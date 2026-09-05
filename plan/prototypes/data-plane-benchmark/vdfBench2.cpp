// Persistent hand-built VdfNetwork prototype for a hair/fur data plane,
// v2: the styler nodes honour the SCHEDULED affects mask of their invocation.
//
// VdfScheduler splits every pool output that passes its buffer into
// ceil(n/500) invocations (grainSize = 500, scheduler.cpp:861) and calls
// Compute() once per invocation. A node that writes the whole buffer therefore
// does O(n^2/500) work and races with the other invocations. v1 (vdfBench.cpp)
// made that mistake on purpose; this version reads the invocation's affects
// mask via VdfIterator::_GetOutputMasks and only touches its own runs.
//
// Two element granularities:
//   --element cv     : element = one GfVec3f control vertex (800k elements).
//                      The 500-element grain does NOT align to 8-CV curves, so
//                      the kernel must be per-CV and may not read a neighbour.
//   --element strip  : element = one whole curve (probe::HairStrip, 8 CVs).
//                      Grain 500 = 500 whole curves; cross-CV kernels are safe.

#include "pxr/pxr.h"

#include "pxr/exec/vdf/context.h"
#include "pxr/exec/vdf/executionTypeRegistry.h"
#include "pxr/exec/vdf/executor.h"
#include "pxr/exec/vdf/inputVector.h"
#include "pxr/exec/vdf/iterator.h"
#include "pxr/exec/vdf/mask.h"
#include "pxr/exec/vdf/maskedOutput.h"
#include "pxr/exec/vdf/network.h"
#include "pxr/exec/vdf/node.h"
#include "pxr/exec/vdf/output.h"
#include "pxr/exec/vdf/parallelDataManagerVector.h"
#include "pxr/exec/vdf/parallelExecutorEngine.h"
#include "pxr/exec/vdf/parallelSpeculationExecutorEngine.h"
#include "pxr/exec/vdf/readWriteIterator.h"
#include "pxr/exec/vdf/request.h"
#include "pxr/exec/vdf/schedule.h"
#include "pxr/exec/vdf/scheduler.h"
#include "pxr/exec/vdf/simpleExecutor.h"
#include "pxr/exec/vdf/vector.h"

#include "pxr/exec/exec/typeRegistry.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/work/threadLimits.h"

#include "kernel.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((pool, ".pool"))
    (amount)
);

namespace {

std::atomic<long> g_computeCalls{0};
std::atomic<long> g_elementsTouched{0};

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int GetArg(int argc, char **argv, const char *name, int def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return atoi(argv[i + 1]);
    return def;
}
const char *GetStrArg(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}

// Raw, range-based access to this invocation's slice of the pool buffer.
template <typename T>
class PoolSlice : public VdfIterator
{
public:
    // Returns the buffer base pointer (indexable by GLOBAL element index) and
    // the scheduled affects mask for this invocation.
    T *Get(const VdfContext &ctx, const TfToken &name, const VdfMask **affects)
    {
        const VdfOutput *o = _GetRequiredOutputForWriting(ctx, name);
        if (!o) return nullptr;
        const VdfMask *req = nullptr;
        if (!_GetOutputMasks(ctx, *o, &req, affects)) return nullptr;
        VdfVector *v = _GetOutputValueForWriting(ctx, *o);
        if (!v) return nullptr;
        typename VdfVector::ReadWriteAccessor<T> a =
            v->template GetReadWriteAccessor<T>();
        if (a.IsEmpty() || a.IsBoxed()) return nullptr;
        return &a[0];
    }
};

template <typename T, typename Fn>
void ForEachAffectedRun(const VdfMask &affects, size_t fallbackSize, Fn &&fn)
{
    if (affects.IsEmpty() || affects.GetSize() == 0) {
        fn(size_t(0), fallbackSize);
        return;
    }
    const VdfMask::Bits &bits = affects.GetBits();
    for (auto it = bits.GetPlatformsView().begin(),
              e = bits.GetPlatformsView().end();
         it != e; ++it) {
        if (!it.IsSet()) continue;
        const size_t b = *it;
        fn(b, b + it.GetPlatformSize());
    }
}

class CvStyler final : public VdfNode
{
public:
    static bool _allocateTrap;
    CvStyler(VdfNetwork *net, size_t numElements, uint32_t seed)
        : VdfNode(net,
              VdfInputSpecs()
                  .ReadWriteConnector<GfVec3f>(_tokens->pool, _tokens->pool)
                  .ReadConnector<float>(_tokens->amount),
              VdfOutputSpecs().Connector<GfVec3f>(_tokens->pool))
        , _seed(seed)
    {
        GetOutput()->SetAffectsMask(VdfMask::AllOnes(numElements));
    }

    void Compute(const VdfContext &ctx) const override
    {
        const float *amt = ctx.GetInputValuePtr<float>(_tokens->amount);
        const float a = amt ? *amt : 0.f;
        PoolSlice<GfVec3f> slice;
        const VdfMask *affects = nullptr;
        GfVec3f *p = slice.Get(ctx, _tokens->pool, &affects);
        if (!p) return;
        g_computeCalls.fetch_add(1, std::memory_order_relaxed);
        if (_allocateTrap) {
            // usdRig trap: Allocate() on a READWRITE/pool output allocates a
            // BOXED value (readWriteIterator.h:215-234 -> allocateBoxedValue.h)
            // and the write silently degrades to pass-through.
            VdfReadWriteIterator<GfVec3f> it =
                VdfReadWriteIterator<GfVec3f>::Allocate(
                    ctx, _tokens->pool, 8);
            (void)it;
        }
        float *f = reinterpret_cast<float *>(p);
        ForEachAffectedRun<GfVec3f>(*affects, 0, [&](size_t b, size_t e) {
            g_elementsTouched.fetch_add(long(e - b),
                                        std::memory_order_relaxed);
            probe::StyleCVRange(f, b, e, a, _seed);
        });
    }

private:
    uint32_t _seed;
};
bool CvStyler::_allocateTrap = false;

class StripStyler final : public VdfNode
{
public:
    StripStyler(VdfNetwork *net, size_t numElements, uint32_t seed)
        : VdfNode(net,
              VdfInputSpecs()
                  .ReadWriteConnector<probe::HairStrip>(_tokens->pool,
                                                        _tokens->pool)
                  .ReadConnector<float>(_tokens->amount),
              VdfOutputSpecs().Connector<probe::HairStrip>(_tokens->pool))
        , _seed(seed)
    {
        GetOutput()->SetAffectsMask(VdfMask::AllOnes(numElements));
    }

    void Compute(const VdfContext &ctx) const override
    {
        const float *amt = ctx.GetInputValuePtr<float>(_tokens->amount);
        const float a = amt ? *amt : 0.f;
        PoolSlice<probe::HairStrip> slice;
        const VdfMask *affects = nullptr;
        probe::HairStrip *p = slice.Get(ctx, _tokens->pool, &affects);
        if (!p) return;
        g_computeCalls.fetch_add(1, std::memory_order_relaxed);
        ForEachAffectedRun<probe::HairStrip>(*affects, 0,
            [&](size_t b, size_t e) {
                g_elementsTouched.fetch_add(long(e - b),
                                            std::memory_order_relaxed);
                probe::StyleStripRange(p, b, e, a, _seed);
            });
    }

private:
    uint32_t _seed;
};

struct Chain {
    std::vector<VdfOutput *> stageOuts;
    Vdf_InputVectorBase *src = nullptr;
    VdfOutput *srcOut = nullptr;
    std::vector<VdfInputVector<float> *> params;
    VdfOutput *terminal = nullptr;
    size_t numElements = 0;
};

Chain BuildChain(VdfNetwork *net, size_t curves, int stylers, bool strip)
{
    Chain ch;
    ch.numElements = strip ? curves : curves * probe::CVS;
    if (strip) {
        auto *s = new VdfInputVector<probe::HairStrip>(net, ch.numElements);
        for (size_t c = 0; c < curves; ++c) {
            probe::HairStrip h;
            for (int k = 0; k < probe::CVS; ++k) {
                h.cv[k][0] = float(c % 512) * 0.01f;
                h.cv[k][1] = float(k) * 0.1f;
                h.cv[k][2] = float(c / 512) * 0.01f;
            }
            s->SetValue(c, h);
        }
        ch.src = s;
        ch.srcOut = s->GetOutput();
    } else {
        auto *s = new VdfInputVector<GfVec3f>(net, ch.numElements);
        for (size_t c = 0; c < curves; ++c)
            for (int k = 0; k < probe::CVS; ++k)
                s->SetValue(c * probe::CVS + k,
                            GfVec3f(float(c % 512) * 0.01f, float(k) * 0.1f,
                                    float(c / 512) * 0.01f));
        ch.src = s;
        ch.srcOut = s->GetOutput();
    }

    const VdfMask all = VdfMask::AllOnes(ch.numElements);
    VdfOutput *head = ch.srcOut;
    for (int s = 0; s < stylers; ++s) {
        VdfNode *node =
            strip ? static_cast<VdfNode *>(
                        new StripStyler(net, ch.numElements, 1u + s * 7919u))
                  : static_cast<VdfNode *>(
                        new CvStyler(net, ch.numElements, 1u + s * 7919u));
        auto *pv = new VdfInputVector<float>(net, 1);
        pv->SetValue(0, 0.25f + 0.05f * s);
        ch.params.push_back(pv);
        net->Connect(head, node, _tokens->pool, all);
        net->Connect(pv->GetOutput(), node, _tokens->amount,
                     VdfMask::AllOnes(1));
        head = node->GetOutput(_tokens->pool);
        ch.stageOuts.push_back(head);
    }
    ch.terminal = head;
    return ch;
}

double Checksum(const VdfExecutorInterface &exec, const Chain &ch, bool strip)
{
    const VdfVector *v =
        exec.GetOutputValue(*ch.terminal, VdfMask::AllOnes(ch.numElements));
    if (!v) return -1.0;
    double s = 0.0;
    if (strip) {
        auto a = v->GetReadAccessor<probe::HairStrip>();
        for (size_t i = 0; i < a.GetNumValues(); i += 122)
            for (int k = 0; k < probe::CVS; ++k)
                s += a[i].cv[k][0] + a[i].cv[k][1] + a[i].cv[k][2];
    } else {
        auto a = v->GetReadAccessor<GfVec3f>();
        for (size_t i = 0; i < a.GetNumValues(); i += 977)
            s += a[i][0] + a[i][1] + a[i][2];
    }
    return s;
}

struct Stat { double lo = 1e30, hi = 0, sum = 0; int n = 0;
    void Add(double v) { lo = std::min(lo, v); hi = std::max(hi, v);
                         sum += v; ++n; }
    void Print(const char *label) const {
        printf("%-32s min %9.3f  med~ %9.3f  max %9.3f  (n=%d)\n",
               label, lo, n ? sum / n : 0.0, hi, n);
    }
};

}  // namespace

TF_REGISTRY_FUNCTION(VdfExecutionTypeRegistry)
{
    VdfExecutionTypeRegistry::Define(probe::HairStrip{});
}

int main(int argc, char **argv)
{
    const int curvesTotal  = GetArg(argc, argv, "--curves", 100000);
    const int stylers      = GetArg(argc, argv, "--stylers", 5);
    const int chains       = GetArg(argc, argv, "--chains", 1);
    const int runs         = GetArg(argc, argv, "--runs", 5);
    const int sparsePermil = GetArg(argc, argv, "--sparse-permil", 10);
    const int simple       = GetArg(argc, argv, "--simple-executor", 0);
    const int nthreads     = GetArg(argc, argv, "--threads", 0);
    const int requestAll   = GetArg(argc, argv, "--request-all", 0);
    const bool strip =
        !strcmp(GetStrArg(argc, argv, "--element", "cv"), "strip");

    CvStyler::_allocateTrap = GetArg(argc, argv, "--allocate-trap", 0) != 0;
    if (nthreads > 0) WorkSetConcurrencyLimit(nthreads);
    else WorkSetMaximumConcurrencyLimit();

    // TRAP (usdRig docs/mover-graph-cutover.md:517-519).
    if (!GetArg(argc, argv, "--skip-type-registry", 0)) {
        ExecTypeRegistry::GetInstance();
        // Our own element type must be defined too, and the
        // TF_REGISTRY_FUNCTION above only fires once the registry singleton is
        // instantiated.
        VdfExecutionTypeRegistry::GetInstance();
        VdfExecutionTypeRegistry::CheckForRegistration<probe::HairStrip>(
            "probe::HairStrip must be defined before building the network");
    } else {
        printf("# NOTE: skipping ExecTypeRegistry/VdfExecutionTypeRegistry "
               "priming\n");
    }

    printf("# vdfBench2 element=%s curves=%d stylers=%d chains=%d "
           "simple=%d threads=%u elemBytes=%zu requestAll=%d\n",
           strip ? "strip" : "cv", curvesTotal, stylers, chains, simple,
           WorkGetConcurrencyLimit(),
           strip ? sizeof(probe::HairStrip) : sizeof(GfVec3f), requestAll);

    VdfNetwork net;
    std::vector<Chain> cs;
    const size_t perChain = size_t(curvesTotal) / size_t(chains);
    auto tb = Clock::now();
    for (int i = 0; i < chains; ++i)
        cs.push_back(BuildChain(&net, perChain, stylers, strip));
    printf("build_network_ms                %9.2f  nodes=%zu\n",
           MsSince(tb), net.GetNodeCapacity());

    VdfMaskedOutputVector ro;
    for (const Chain &ch : cs) {
        if (requestAll) {
            // scheduler.cpp:426-432 gives every REQUESTED output keepMask =
            // requestMask, which stops it from passing its buffer: this is the
            // lever that turns a pool chain into per-node cached results.
            for (VdfOutput *o : ch.stageOuts)
                ro.push_back(VdfMaskedOutput(o,
                    VdfMask::AllOnes(ch.numElements)));
        } else {
            ro.push_back(VdfMaskedOutput(ch.terminal,
                VdfMask::AllOnes(ch.numElements)));
        }
    }
    VdfRequest request(ro);

    VdfSchedule schedule;
    auto ts = Clock::now();
    VdfScheduler::Schedule(request, &schedule, true);
    printf("schedule_first_ms               %9.3f\n", MsSince(ts));

    std::unique_ptr<VdfExecutorInterface> ep;
    if (simple) ep = std::make_unique<VdfSimpleExecutor>();
    else ep = std::make_unique<VdfExecutor<VdfParallelExecutorEngine,
                                           VdfParallelDataManagerVector>>();
    VdfExecutorInterface &exec = *ep;
    exec.Resize(net);

    {
        auto t0 = Clock::now();
        exec.Run(schedule);
        printf("run_cold_ms                     %9.2f  checksum=%.4f\n",
               MsSince(t0), Checksum(exec, cs[0], strip));
        printf("  compute_calls=%ld elements_touched=%ld "
               "(ideal=%ld nodes x %zu elems)\n",
               g_computeCalls.load(), g_elementsTouched.load(),
               long(stylers) * chains, cs[0].numElements);
    }

    Stat noop, full, sparseS, scatterS, lastP, firstP;
    for (int r = 0; r < runs; ++r) {
        auto t0 = Clock::now(); exec.Run(schedule); noop.Add(MsSince(t0));
    }
    for (int r = 0; r < runs; ++r) {
        VdfMaskedOutputVector inv;
        for (const Chain &ch : cs)
            inv.push_back(VdfMaskedOutput(ch.srcOut,
                                          VdfMask::AllOnes(ch.numElements)));
        exec.InvalidateValues(inv);
        g_computeCalls = 0; g_elementsTouched = 0;
        auto t0 = Clock::now(); exec.Run(schedule); full.Add(MsSince(t0));
    }
    const long fullTouched = g_elementsTouched.load();

    const size_t nElem = cs[0].numElements;
    const size_t elemsPerCurve = strip ? 1 : probe::CVS;
    const size_t hitCurves = (perChain * size_t(sparsePermil)) / 1000;
    VdfMask::Bits cb(nElem);
    for (size_t i = 0; i < hitCurves * elemsPerCurve; ++i) cb.Set(i);
    VdfMask contiguous(std::move(cb));
    VdfMask::Bits sb(nElem);
    const size_t stride = 1000 / (sparsePermil ? sparsePermil : 1);
    for (size_t c = 0; c < perChain; c += stride)
        for (size_t k = 0; k < elemsPerCurve; ++k)
            sb.Set(c * elemsPerCurve + k);
    VdfMask scattered(std::move(sb));
    printf("sparse masks: contiguous %zu/%zu  scattered %zu/%zu\n",
           contiguous.GetNumSet(), nElem, scattered.GetNumSet(), nElem);

    long sparseTouched = 0, scatterTouched = 0;
    for (int r = 0; r < runs; ++r) {
        exec.InvalidateValues({VdfMaskedOutput(cs[0].srcOut, contiguous)});
        g_computeCalls = 0; g_elementsTouched = 0;
        auto t0 = Clock::now(); exec.Run(schedule); sparseS.Add(MsSince(t0));
        sparseTouched = g_elementsTouched.load();
    }
    for (int r = 0; r < runs; ++r) {
        exec.InvalidateValues({VdfMaskedOutput(cs[0].srcOut, scattered)});
        g_computeCalls = 0; g_elementsTouched = 0;
        auto t0 = Clock::now(); exec.Run(schedule); scatterS.Add(MsSince(t0));
        scatterTouched = g_elementsTouched.load();
    }
    for (int r = 0; r < runs; ++r) {
        cs[0].params[stylers - 1]->SetValue(0, 0.3f + 0.001f * r);
        exec.InvalidateValues({VdfMaskedOutput(
            cs[0].params[stylers - 1]->GetOutput(), VdfMask::AllOnes(1))});
        g_elementsTouched = 0;
        auto t0 = Clock::now(); exec.Run(schedule); lastP.Add(MsSince(t0));
    }
    const long lastTouched = g_elementsTouched.load();
    for (int r = 0; r < runs; ++r) {
        cs[0].params[0]->SetValue(0, 0.2f + 0.001f * r);
        exec.InvalidateValues({VdfMaskedOutput(cs[0].params[0]->GetOutput(),
                                               VdfMask::AllOnes(1))});
        g_elementsTouched = 0;
        auto t0 = Clock::now(); exec.Run(schedule); firstP.Add(MsSince(t0));
    }
    const long firstTouched = g_elementsTouched.load();

    noop.Print("run_cached_noop_ms");
    full.Print("run_full_ms");
    sparseS.Print("run_after_sparse_contig_ms");
    scatterS.Print("run_after_sparse_scatter_ms");
    lastP.Print("run_after_last_param_ms");
    firstP.Print("run_after_first_param_ms");
    printf("elements_touched: full=%ld contig=%ld scatter=%ld "
           "lastparam=%ld firstparam=%ld\n",
           fullTouched, sparseTouched, scatterTouched, lastTouched,
           firstTouched);

    // Sparse EXECUTION: the schedule's affects masks are fixed at schedule
    // time from the request mask, so narrowing the request is the only way to
    // make a run touch fewer elements.
    {
        VdfSchedule narrow;
        auto t0 = Clock::now();
        VdfScheduler::Schedule(
            VdfRequest(VdfMaskedOutput(cs[0].terminal, contiguous)),
            &narrow, true);
        const double sched = MsSince(t0);
        exec.InvalidateValues({VdfMaskedOutput(cs[0].srcOut, contiguous)});
        g_computeCalls = 0; g_elementsTouched = 0;
        auto t1 = Clock::now();
        exec.Run(narrow);
        const double run = MsSince(t1);
        printf("narrow_schedule_ms %8.3f  narrow_run_ms %8.3f  "
               "touched=%ld calls=%ld\n",
               sched, run, g_elementsTouched.load(), g_computeCalls.load());
        // Restore full validity for the following measurements.
        exec.InvalidateValues({VdfMaskedOutput(cs[0].srcOut,
            VdfMask::AllOnes(nElem))});
        exec.Run(schedule);
    }

    // Extraction (cv mode only: GfVec3f -> VtVec3fArray).
    if (!strip) {
        const VdfMask all = VdfMask::AllOnes(nElem);
        const VdfVector *v = exec.GetOutputValue(*cs[0].terminal, all);
        Stat cp, sh;
        for (int r = 0; r < runs; ++r) {
            auto t0 = Clock::now();
            VtArray<GfVec3f> arr = v->ExtractAsVtArray<GfVec3f>(nElem, 0);
            cp.Add(MsSince(t0));
            if (arr.size() != nElem) printf("BAD SIZE\n");
        }
        printf("vector sharable=%d shared=%d -> Share()=%d\n",
               (int)v->IsSharable(), (int)v->IsShared(), (int)v->Share());
        for (int r = 0; r < runs; ++r) {
            auto t0 = Clock::now();
            VtArray<GfVec3f> arr = v->ExtractAsVtArray<GfVec3f>(nElem, 0);
            sh.Add(MsSince(t0));
            if (arr.size() != nElem) printf("BAD SIZE\n");
        }
        cp.Print("extract_copy_ms");
        sh.Print("extract_after_share_ms");
        exec.InvalidateValues({VdfMaskedOutput(cs[0].srcOut, all)});
        auto t0 = Clock::now();
        exec.Run(schedule);
        printf("run_full_after_share_ms         %9.2f\n", MsSince(t0));
    }

    // Topology edit -> reschedule.
    {
        VdfNode *extra =
            strip ? static_cast<VdfNode *>(new StripStyler(&net, nElem, 42u))
                  : static_cast<VdfNode *>(new CvStyler(&net, nElem, 42u));
        auto *pv = new VdfInputVector<float>(&net, 1);
        pv->SetValue(0, 0.1f);
        net.Connect(cs[0].terminal, extra, _tokens->pool,
                    VdfMask::AllOnes(nElem));
        net.Connect(pv->GetOutput(), extra, _tokens->amount,
                    VdfMask::AllOnes(1));
        cs[0].terminal = extra->GetOutput(_tokens->pool);
        printf("schedule_valid_after_topology_edit  %d\n",
               (int)schedule.IsValid());
        VdfMaskedOutputVector ro2;
        for (const Chain &ch : cs)
            ro2.push_back(VdfMaskedOutput(ch.terminal,
                                          VdfMask::AllOnes(ch.numElements)));
        VdfRequest rq2(ro2);
        auto t0 = Clock::now();
        VdfScheduler::Schedule(rq2, &schedule, true);
        const double sms = MsSince(t0);
        auto t1 = Clock::now();
        exec.InvalidateTopologicalState();
        const double tms = MsSince(t1);
        exec.Resize(net);
        g_elementsTouched = 0;
        auto t2 = Clock::now();
        exec.Run(schedule);
        printf("reschedule_ms %8.3f  invalidate_topo_ms %8.3f  "
               "run_after_edit_ms %8.2f  touched=%ld\n",
               sms, tms, MsSince(t2), g_elementsTouched.load());
    }

    // Schedule cost as a function of node count.
    for (int target : {100, 1000, 5000}) {
        VdfNetwork n2;
        const size_t elems = 8 * 1024;
        std::vector<VdfOutput *> terms;
        const int per = 5, nch = target / per;
        for (int i = 0; i < nch; ++i) {
            auto *src = new VdfInputVector<GfVec3f>(&n2, elems);
            VdfOutput *head = src->GetOutput();
            for (int s = 0; s < per; ++s) {
                auto *nd = new CvStyler(&n2, elems, s);
                auto *pv = new VdfInputVector<float>(&n2, 1);
                pv->SetValue(0, 0.5f);
                n2.Connect(head, nd, _tokens->pool, VdfMask::AllOnes(elems));
                n2.Connect(pv->GetOutput(), nd, _tokens->amount,
                           VdfMask::AllOnes(1));
                head = nd->GetOutput(_tokens->pool);
            }
            terms.push_back(head);
        }
        VdfMaskedOutputVector r3;
        for (VdfOutput *o : terms)
            r3.push_back(VdfMaskedOutput(o, VdfMask::AllOnes(elems)));
        VdfRequest rq(r3);
        Stat st;
        for (int r = 0; r < runs; ++r) {
            VdfSchedule sc;
            auto t0 = Clock::now();
            VdfScheduler::Schedule(rq, &sc, true);
            st.Add(MsSince(t0));
        }
        char label[64];
        snprintf(label, sizeof(label), "schedule_%zu_nodes_ms",
                 n2.GetNodeCapacity());
        st.Print(label);
    }
    return 0;
}
