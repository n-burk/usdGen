// SPDX-License-Identifier: Apache-2.0
// benchUsdGenStorm - M1 storm timing-gate driver (plan/09 S-1/S-5/S-6/S-12).
//
// UsdImagingGLEngine + HdStormRendererPlugin on a headless EGL device context
// (usdGenShaders/test/eglctx.h). Timing (plan/09 4.5): per repeat, `warmup`
// frames then `frames` timed; the first (cold) repeat is discarded; median is
// the median of per-repeat medians, p90/min over retained frame samples.
// Counters from Engine::GetRenderStats() via lowercase-substring key mapping;
// a missing counter is JSON null, never 0. Thresholds live ctest-side.
#include "eglctx.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/perfLog.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

typedef void (*PFNGLFINISH)();

PFNGLFINISH ResolveGLFinish()
{
    PFNGLFINISH p = (PFNGLFINISH)dlsym(RTLD_DEFAULT, "glFinish");
    if (!p) {
        void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (gl) p = (PFNGLFINISH)dlsym(gl, "glFinish");
    }
    if (!p) {
        void* egl = dlopen("libEGL.so.1", RTLD_NOW);
        typedef void* (*PFNGPA)(const char*);
        PFNGPA gpa = egl ? (PFNGPA)dlsym(egl, "eglGetProcAddress") : nullptr;
        if (gpa) p = (PFNGLFINISH)gpa("glFinish");
    }
    return p;
}

double MedianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
double P90Of(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0.0;
    size_t i = (size_t)std::ceil(0.9 * (v.size() - 1));
    return v[std::min(i, v.size() - 1)];
}

// ---- stats flattening + counter mapping ---------------------------------
struct FlatStats {
    std::vector<std::pair<std::string, double>> numeric;  // lowercased keys
    std::vector<std::pair<std::string, std::string>> all;
};
// HdPerfLog counters (drawBatches/drawCalls/itemsDrawn/...) are NOT part of
// UsdImagingGLEngine::GetRenderStats (resource counters only). The
// counter-accurate path (prototypes/storm-throughput/hairbench.cpp:85-92)
// is HdPerfLog::GetInstance().Enable() with HD_ENABLE_PERFLOG=1 in the
// environment; warn when the env gate is unset since every counter then
// reads 0. Callers must Disable() + ResetCounters() at teardown so sibling
// tests' perflog is not polluted.
void EnablePerfLog()
{
    if (!std::getenv("HD_ENABLE_PERFLOG")) {
        std::fprintf(stderr,
                     "WARNING: HD_ENABLE_PERFLOG=1 is not set; HdPerfLog "
                     "counters will read 0 (pxr/imaging/hd/perfLog.cpp)\n");
    }
    HdPerfLog::GetInstance().Enable();
}

void TeardownPerfLog()
{
    HdPerfLog::GetInstance().ResetCounters();
    HdPerfLog::GetInstance().Disable();
}

// Reads one HdPerfLog counter by token; returns false when the counter is
// absent or zero (indistinguishable from disabled perflog — callers emit
// JSON null in that case, never 0).
bool PerfCounter(const TfToken &tok, double *val)
{
    const double v = HdPerfLog::GetInstance().GetCounter(tok);
    if (v == 0.0) {
        return false;
    }
    *val = v;
    return true;
}

bool ValueToNumber(const VtValue& v, double* out)
{
    // int64_t is `long` on LP64; storm counts arrive as size_t/long long/int.
    if (v.IsHolding<size_t>())    { *out = (double)v.Get<size_t>(); return true; }
    if (v.IsHolding<long long>()) { *out = (double)v.Get<long long>(); return true; }
    if (v.IsHolding<unsigned>())  { *out = (double)v.Get<unsigned>(); return true; }
    if (v.IsHolding<int>())       { *out = (double)v.Get<int>(); return true; }
    if (v.IsHolding<double>())    { *out = v.Get<double>(); return true; }
    if (v.IsHolding<float>())     { *out = (double)v.Get<float>(); return true; }
    if (v.IsHolding<bool>())      { *out = v.Get<bool>() ? 1 : 0; return true; }
    return false;
}

std::string ToLower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void Flatten(const VtDictionary& dict, const std::string& prefix,
             FlatStats* out)
{
    for (const auto& kv : dict) {
        std::string key = prefix.empty() ? kv.first : prefix + "/" + kv.first;
        if (kv.second.IsHolding<VtDictionary>()) {
            Flatten(kv.second.Get<VtDictionary>(), key, out);
            continue;
        }
        double d = 0.0;
        std::string text;
        if (ValueToNumber(kv.second, &d)) {
            char b[64];
            std::snprintf(b, sizeof(b), "%lld", (long long)d);
            text = b;
            out->numeric.emplace_back(ToLower(key), d);
        } else {
            text = kv.second.GetTypeName();
        }
        out->all.emplace_back(key, text);
    }
}

bool FindCounter(const FlatStats& fs, const char* const* subs, double* val)
{
    for (const char* const* s = subs; *s; ++s)
        for (const auto& kv : fs.numeric)
            if (kv.first.find(*s) != std::string::npos) {
                *val = kv.second;
                return true;
            }
    return false;
}

// HdPerfLog token reads: drawBatches/drawCalls/itemsDrawn live here, not in
// GetRenderStats. Reset before a run so the count covers the run only.
struct PerfCounts {
    bool haveBatches = false, haveCalls = false, haveItems = false;
    double batches = 0.0, calls = 0.0, items = 0.0;
};

PerfCounts ReadPerfCounts()
{
    PerfCounts pc;
    pc.haveBatches =
        PerfCounter(HdPerfTokens->drawBatches, &pc.batches);
    pc.haveCalls = PerfCounter(HdPerfTokens->drawCalls, &pc.calls);
    pc.haveItems = PerfCounter(HdTokens->itemsDrawn, &pc.items);
    return pc;
}

struct NamedCounter { const char* name; const char* subs[3]; };
// lower-case substrings of storm's stats keys; first candidate with a hit wins
const NamedCounter kCounters[] = {
    { "drawCalls",          { "drawcall", 0, 0 } },
    { "drawBatches",        { "drawbatch", 0, 0 } },
    { "rebuildBatches",     { "rebuildbatch", 0, 0 } },
    { "vboRelocated",       { "relocat", 0, 0 } },
    { "itemsDrawn",         { "itemsdrawn", 0, 0 } },
    { "sourcesCommitted",   { "committed", 0, 0 } },
    { "copyBufferCpuToGpu", { "cputogpu", 0, 0 } },
    { "gpuMemoryUsed",      { "gpumemory", "glresourcemem", 0 } },
};
const size_t kNumCounters = sizeof(kCounters) / sizeof(kCounters[0]);

std::string Num(double v)
{
    char b[64];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

// USDGEN_STATS env ("key=value" tokens, sep , ; or space; see
// libs/usdGen/usdGen/stats.h): commits/cookedChunks when published.
std::string EnvStat(const char* key)
{
    const char* s = std::getenv("USDGEN_STATS");
    if (!s) return "null";
    std::string all(s), pre(key);
    pre += "=";
    size_t pos = 0;
    while ((pos = all.find(pre, pos)) != std::string::npos) {
        bool atSep = pos == 0 ||
            all.find_first_of(" ,;", pos - 1) != std::string::npos;
        pos += pre.size();
        if (!atSep) continue;
        size_t e = all.find_first_of(" ,;", pos);
        std::string tok = all.substr(pos, e == std::string::npos
                                            ? std::string::npos : e - pos);
        char* end = nullptr;
        double d = std::strtod(tok.c_str(), &end);
        if (end && end != tok.c_str() && *end == '\0')
            return Num(std::llround(d));
        pos = pos + tok.size();
    }
    return "null";
}

std::string BaseName(const std::string& path)
{
    size_t s = path.find_last_of('/');
    std::string b = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

std::string HostName()
{
    char b[256] = { 0 };
    return gethostname(b, sizeof(b) - 1) == 0 ? std::string(b) : "unknown";
}

int ThreadCount()
{
    const char* e = std::getenv("PXR_WORK_THREAD_LIMIT");
    if (e && *e) { int n = std::atoi(e); if (n > 0) return n; }
    unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 1;
}

void DumpCounters(const FlatStats& fs)
{
    std::fprintf(stderr, "--- GetRenderStats() flattened ---\n");
    for (const auto& kv : fs.all)
        std::fprintf(stderr, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
}

// ---- scene + measurement -------------------------------------------------
struct Scene {
    UsdStageRefPtr stage;
    UsdPrim root;
    int tiles = 0;
    SdfPath camPath;
    double t0 = 0, t1 = 0;
    bool animated = false;
};

bool OpenScene(const std::string& path, Scene* out)
{
    out->stage = UsdStage::Open(path);
    if (!out->stage) return false;
    UsdPrim def = out->stage->GetDefaultPrim();
    out->root = def ? def : out->stage->GetPseudoRoot();
    for (const UsdPrim& p : UsdPrimRange(out->stage->GetPseudoRoot())) {
        if (p.IsA<UsdGeomBasisCurves>()) ++out->tiles;
        else if (p.IsA<UsdGeomCamera>() && out->camPath.IsEmpty())
            out->camPath = p.GetPath();
    }
    out->t0 = out->stage->GetStartTimeCode();
    out->t1 = out->stage->GetEndTimeCode();
    out->animated = out->t1 > out->t0;
    return true;
}

struct RunResult {
    std::vector<double> kept;   // per-frame ms, cold repeat discarded
    std::vector<double> medians;
    FlatStats stats;
    PerfCounts perf;  // HdPerfLog drawBatches/drawCalls/itemsDrawn, this run
};

// Renders warmup+timed frames; `cycleTime` steps params.frame per frame
// (deform). Engine must already have GL current + buffer size set.
RunResult Measure(UsdImagingGLEngine* engine, const Scene& scene, int frames,
                  int warmup, int repeats, int refineLevel, bool cycleTime,
                  PFNGLFINISH glFinish)
{
    RunResult rr;
    UsdImagingGLRenderParams params;
    params.complexity = 1.0f + 0.5f * (float)refineLevel;
    const int span = scene.animated ? (int)(scene.t1 - scene.t0) + 1 : 1;
    long long frameNo = 0;

    auto renderOne = [&](bool timed, std::vector<double>* samples) {
        if (cycleTime && scene.animated)
            params.frame = UsdTimeCode(scene.t0 + (double)(frameNo % span));
        ++frameNo;
        auto t = std::chrono::steady_clock::now();
        engine->Render(scene.root, params);
        if (glFinish) glFinish();
        if (timed)
            samples->push_back(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t).count());
    };

    for (int i = 0; i < warmup; ++i) renderOne(false, nullptr);
    // camera path after population (usdview order)
    if (!scene.camPath.IsEmpty()) engine->SetCameraPath(scene.camPath);

    HdPerfLog::GetInstance().ResetCounters();
    for (int r = 0; r < std::max(repeats, 1); ++r) {
        for (int i = 0; i < warmup; ++i) renderOne(false, nullptr);
        std::vector<double> samples;
        samples.reserve(frames);
        for (int i = 0; i < frames; ++i) renderOne(true, &samples);
        if (r > 0) {  // discard the cold repeat entirely
            rr.kept.insert(rr.kept.end(), samples.begin(), samples.end());
            rr.medians.push_back(MedianOf(samples));
        } else if (repeats <= 1) {  // nothing else kept: salvage single run
            rr.kept = samples;
            rr.medians.push_back(MedianOf(samples));
        }
    }
    Flatten(engine->GetRenderStats(), "", &rr.stats);
    rr.perf = ReadPerfCounts();
    return rr;
}

void AppendSchemaFields(std::string* j, const std::string& sceneName,
                        const char* gate, int w, int h, int refineLevel,
                        int tiles, int frames, int warmup, int repeats)
{
    *j += "{\n";
    *j += "  \"scene\": \"" + sceneName + "\", \"tier\": \"T2\", \"gate\": \"" + gate + "\",\n";
    *j += "  \"host\": \"" + HostName() + "\", \"resolution\": [" + Num(w) +
          ", " + Num(h) + "],\n";
    *j += "  \"refineLevel\": " + Num(refineLevel) + ", \"tiles\": " + Num(tiles) +
          ", \"threads\": " + Num(ThreadCount()) + ",\n";
    *j += "  \"frames\": " + Num(frames) + ", \"warmup\": " + Num(warmup) +
          ", \"repeats\": " + Num(repeats) + ",\n";
}

void AppendMs(std::string* j, const RunResult& rr, const char* indent)
{
    if (rr.kept.empty()) { *j += std::string(indent) + "\"ms\": null,\n"; return; }
    *j += std::string(indent) + "\"ms\": { \"median\": " +
          Num(MedianOf(rr.medians.empty() ? rr.kept : rr.medians)) +
          ", \"p90\": " + Num(P90Of(rr.kept)) +
          ", \"min\": " + Num(*std::min_element(rr.kept.begin(), rr.kept.end())) +
          " },\n";
}

void AppendCounters(std::string* j, const FlatStats& fs, const PerfCounts& pc,
                    const char* indent)
{
    *j += std::string(indent) + "\"counters\": {";
    for (size_t i = 0; i < kNumCounters; ++i) {
        double v = 0.0;
        bool have = FindCounter(fs, kCounters[i].subs, &v);
        // HdPerfLog is authoritative for drawBatches/drawCalls/itemsDrawn:
        // GetRenderStats never carries them (resource counters only).
        if (!have && std::strcmp(kCounters[i].name, "drawBatches") == 0 &&
            pc.haveBatches) {
            v = pc.batches;
            have = true;
        }
        if (!have && std::strcmp(kCounters[i].name, "drawCalls") == 0 &&
            pc.haveCalls) {
            v = pc.calls;
            have = true;
        }
        if (!have && std::strcmp(kCounters[i].name, "itemsDrawn") == 0 &&
            pc.haveItems) {
            v = pc.items;
            have = true;
        }
        *j += (i ? ", " : " ") + TfStringPrintf("\"%s\": ", kCounters[i].name) +
              (have ? Num(std::llround(v)) : std::string("null"));
    }
    *j += " },\n";
}

void AppendUsdGenStats(std::string* j, const char* indent)
{
    // Engine-side usdGen stats hook lands later: only commits/cookedChunks
    // may exist (via USDGEN_STATS env publication); everything else null.
    *j += std::string(indent) + "\"usdGenStats\": { \"commits\": " +
          EnvStat("commits") + ", \"cookedChunks\": " + EnvStat("cookedChunks") +
          ", \"publishedTiles\": null, \"interleavedBytes\": null, "
          "\"recompiles\": null, \"evictions\": null },\n";
}

}  // namespace

int main(int argc, char** argv)
{
    // UsdGeom schemas self-register via static init; Storm loads by id below.
    std::string scenePath, jsonOut = "usdGenBench.json", sceneList;
    const char* gate = "S-1";
    const char* mode = "static";
    int w = 1280, h = 720, refineLevel = 1, frames = 60, warmup = 10, repeats = 3;
    bool countersDump = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--scene") scenePath = next("--scene");
        else if (a == "--scene-list") sceneList = next("--scene-list");
        else if (a == "--static") { mode = "static"; gate = "S-1"; }
        else if (a == "--batches") { mode = "batches"; gate = "S-5"; }
        else if (a == "--deform") { mode = "deform"; gate = "S-6"; }
        else if (a == "--tilesweep") { mode = "tilesweep"; gate = "S-12"; }
        else if (a == "--res") {
            if (std::sscanf(next("--res"), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                std::fprintf(stderr, "bad --res (want WxH)\n");
                return 2;
            }
        }
        else if (a == "--refine") refineLevel = std::atoi(next("--refine"));
        else if (a == "--frames") frames = std::atoi(next("--frames"));
        else if (a == "--warmup") warmup = std::atoi(next("--warmup"));
        else if (a == "--repeats") repeats = std::atoi(next("--repeats"));
        else if (a == "--json") jsonOut = next("--json");
        else if (a == "--counters") countersDump = true;
        else {
            std::fprintf(stderr,
                "usage: benchUsdGenStorm --scene PATH [--static|--batches|--deform]"
                " [--tilesweep --scene-list p1,p2] [--res WxH] [--refine {1,2}]"
                " [--frames N] [--warmup N] [--repeats N] [--json OUT] [--counters]\n");
            return 2;
        }
    }
    if (mode != "tilesweep" && scenePath.empty()) {
        return 2;
    }
    if (refineLevel < 0 || refineLevel > 2) refineLevel = 1;

    // Fail hard if the Storm plugin is not installed.
    {
        if (!HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(
                TfToken("HdStormRendererPlugin"))) {
            std::fprintf(stderr, "FATAL: HdStormRendererPlugin not registered\n");
            return 1;
        }
    }

    // Headless GL: EGL device context must be current before engine ctor.
    if (!eglctx::MakeHeadlessGLContext()) {
        std::fprintf(stderr, "FATAL: no headless EGL device context available\n");
        return 1;
    }
    PFNGLFINISH glFinish = ResolveGLFinish();
    if (!glFinish)
        std::fprintf(stderr, "WARN: glFinish unresolved; timings CPU-side only\n");

    UsdImagingGLEngine engine(HdDriver(), TfToken("HdStormRendererPlugin"), true);
    engine.SetEnablePresentation(false);
    engine.SetRenderBufferSize(GfVec2i(w, h));
    // Storm stats collection: setting first, command fallback if empty.
    engine.SetRendererSetting(TfToken("collectStats"), VtValue(true));
    // HdPerfLog carries drawBatches/drawCalls/itemsDrawn (GetRenderStats
    // does not); torn down before every return below.
    EnablePerfLog();

    std::string json;
    double ratioVsOne = -1.0;

    if (mode == "tilesweep") {
        if (sceneList.empty()) {
            std::fprintf(stderr, "--tilesweep requires --scene-list p1,p2,...\n");
            return 2;
        }
        json = "{\n  \"gate\": \"S-12\", \"tier\": \"T2\", \"host\": \"" + HostName() +
               "\", \"resolution\": [" + Num(w) + ", " + Num(h) + "],\n" +
               "  \"refineLevel\": " + Num(refineLevel) + ", \"threads\": " +
               Num(ThreadCount()) + ", \"frames\": " + Num(frames) +
               ", \"warmup\": " + Num(warmup) + ", \"repeats\": " + Num(repeats) +
               ",\n  \"scenes\": [";
        std::vector<std::string> paths = TfStringSplit(sceneList, ",");
        double baseBatches = 0.0;
        bool first = true;
        for (size_t k = 0; k < paths.size(); ++k) {
            std::string p = TfStringTrim(paths[k]);
            Scene sc;
            if (!OpenScene(p, &sc)) {
                std::fprintf(stderr, "FATAL: cannot open %s\n", p.c_str());
                TeardownPerfLog();
                return 1;
            }
            RunResult rr = Measure(&engine, sc, frames, warmup, repeats,
                                   refineLevel, false, glFinish);
            double batches = 0.0;
            const char* subs[] = { "drawbatch", 0 };
            bool haveBatches = FindCounter(rr.stats, subs, &batches);
            if (!haveBatches && rr.perf.haveBatches) {
                batches = rr.perf.batches;
                haveBatches = true;
            }
            std::string one;
            AppendSchemaFields(&one, BaseName(p), gate, w, h, refineLevel,
                               sc.tiles, frames, warmup, repeats);
            AppendMs(&one, rr, "  ");
            one += "  ";
            AppendCounters(&one, rr.stats, rr.perf, "  ");
            AppendUsdGenStats(&one, "  ");
            if (haveBatches && baseBatches > 0.0)
                one += "  \"drawBatchesRatioVsOnePrim\": " +
                       Num(batches / baseBatches) + ",\n";
            // strip trailing ",\n" from last element
            std::fprintf(stderr, "tilesweep %s: tiles=%d median %.3f ms batches %s\n",
                         BaseName(p).c_str(), sc.tiles,
                         rr.kept.empty() ? 0.0 : MedianOf(rr.medians.empty()
                             ? rr.kept : rr.medians),
                         haveBatches ? Num(std::llround(batches)).c_str() : "null");
            // remove trailing comma+newline if present
            while (!one.empty() && (one.back() == '\n' || one.back() == ',' ||
                                    one.back() == ' '))
                one.pop_back();
            json += (first ? "\n    " : ",\n    ") + one;
            first = false;
            if (countersDump) DumpCounters(rr.stats);
        }
        json += "\n  ]\n}\n";
        if (!scenePath.empty()) { /* --scene optional alongside list */ }
    } else {
        Scene sc;
        if (!OpenScene(scenePath, &sc)) {
            std::fprintf(stderr, "FATAL: cannot open %s\n", scenePath.c_str());
            TeardownPerfLog();
            return 1;
        }
        bool cycleTime = (mode == "deform");
        RunResult rr = Measure(&engine, sc, frames, warmup, repeats,
                               refineLevel, cycleTime, glFinish);
        // stats-enable fallback: if dict still empty, try the console command
        if (rr.stats.all.empty()) {
            HdCommandArgs args;
            engine.InvokeRendererCommand(TfToken("toggleStatsCollection"), args);
            rr = Measure(&engine, sc, std::min(frames, 5), 1, 1, refineLevel,
                         cycleTime, glFinish);
        }
        if (countersDump) DumpCounters(rr.stats);
        AppendSchemaFields(&json, BaseName(scenePath), gate, w, h, refineLevel,
                           sc.tiles, frames, warmup, repeats);
        AppendMs(&json, rr, "  ");
        AppendCounters(&json, rr.stats, rr.perf, "  ");
        AppendUsdGenStats(&json, "  ");
        json += "}\n";
        // human line (gate expectations live ctest-side; never assert here)
        double batches = rr.perf.haveBatches ? rr.perf.batches : 0.0;
        double calls = rr.perf.haveCalls ? rr.perf.calls : 0.0;
        const char* bsubs[] = { "drawbatch", 0 };
        const char* csubs[] = { "drawcall", 0 };
        bool hb = rr.perf.haveBatches ||
                  FindCounter(rr.stats, bsubs, &batches);
        bool hc = rr.perf.haveCalls || FindCounter(rr.stats, csubs, &calls);
        std::printf("%s median %s ms (p90 %s min %s) drawBatches=%s drawCalls=%s\n",
                    gate,
                    Num(rr.kept.empty() ? 0.0
                        : MedianOf(rr.medians.empty() ? rr.kept : rr.medians)).c_str(),
                    Num(rr.kept.empty() ? 0.0 : P90Of(rr.kept)).c_str(),
                    Num(rr.kept.empty() ? 0.0
                        : *std::min_element(rr.kept.begin(), rr.kept.end())).c_str(),
                    hb ? Num(std::llround(batches)).c_str() : "null",
                    hc ? Num(std::llround(calls)).c_str() : "null");
    }

    std::ofstream ofs(jsonOut);
    if (!ofs) {
        std::fprintf(stderr, "FATAL: cannot write %s\n", jsonOut.c_str());
        TeardownPerfLog();
        return 1;
    }
    ofs << json;
    ofs.close();
    TeardownPerfLog();
    return 0;
}
   