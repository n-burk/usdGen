// Minimal in-process SIGPROF sampler: this host blocks perf and ptrace attach
// (/proc/sys/kernel/perf_event_paranoid=4, yama ptrace_scope), so the probe
// profiles itself.
#ifndef USDGEN_PROBE_SAMPLER_H
#define USDGEN_PROBE_SAMPLER_H

#include <cxxabi.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <algorithm>
#include <vector>

namespace probe {

static constexpr int kMaxFrames = 24;
static constexpr int kMaxSamples = 40000;

struct Sampler {
    static std::atomic<int> on;
    static std::atomic<int> count;
    static void *frames[kMaxSamples][kMaxFrames];
    static int depth[kMaxSamples];
};

std::atomic<int> Sampler::on{0};
std::atomic<int> Sampler::count{0};
void *Sampler::frames[kMaxSamples][kMaxFrames];
int Sampler::depth[kMaxSamples];

inline void OnProf(int)
{
    if (!Sampler::on.load(std::memory_order_relaxed)) return;
    const int i = Sampler::count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    Sampler::depth[i] = backtrace(Sampler::frames[i], kMaxFrames);
}

inline void SamplerStart(int usec = 2000)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnProf;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = usec;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, nullptr);
    Sampler::on.store(1);
}

inline std::string Demangled(const char *sym)
{
    // backtrace_symbols gives "path(mangled+0xoff) [addr]"
    const char *o = strchr(sym, '(');
    if (!o) return sym;
    const char *p = strchr(o, '+');
    if (!p) p = strchr(o, ')');
    if (!p) return sym;
    std::string mangled(o + 1, p);
    if (mangled.empty()) return "<unknown>";
    int st = 0;
    char *dem = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &st);
    std::string r = (st == 0 && dem) ? dem : mangled;
    free(dem);
    return r;
}

inline void SamplerReport(const char *label, int topN = 18)
{
    Sampler::on.store(0);
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, nullptr);

    const int n = std::min<int>(Sampler::count.load(), kMaxSamples);
    std::map<std::string, int> leaf, incl;
    for (int i = 0; i < n; ++i) {
        char **syms = backtrace_symbols(Sampler::frames[i], Sampler::depth[i]);
        if (!syms) continue;
        // Frame 0/1 are the signal handler + libc trampoline.
        bool first = true;
        std::map<std::string, bool> seen;
        for (int f = 0; f < Sampler::depth[i]; ++f) {
            std::string s = Demangled(syms[f]);
            if (s.find("probe::OnProf") != std::string::npos ||
                s.find("__kernel") != std::string::npos ||
                s.find("libc.so") != std::string::npos) {
                continue;
            }
            if (first) { leaf[s]++; first = false; }
            if (!seen[s]) { incl[s]++; seen[s] = true; }
        }
        free(syms);
    }
    printf("\n### sampler %s: %d samples\n", label, n);
    auto dump = [&](const char *what, std::map<std::string, int> &m) {
        std::vector<std::pair<int, std::string>> v;
        for (auto &kv : m) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        printf("-- %s\n", what);
        for (int i = 0; i < (int)v.size() && i < topN; ++i) {
            printf("%6.1f%%  %s\n", 100.0 * v[i].first / (n ? n : 1),
                   v[i].second.substr(0, 150).c_str());
        }
    };
    dump("self (leaf frame)", leaf);
    dump("inclusive (anywhere in stack)", incl);
    Sampler::count.store(0);
}

}  // namespace probe

#endif
