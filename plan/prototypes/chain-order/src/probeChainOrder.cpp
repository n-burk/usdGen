//
// G-chain-order probe.
//
// 1. Prints the std::set<TfType> iteration order that
//    UsdImagingSceneIndexPlugin::GetAllSceneIndexPlugins() walks
//    (usdImaging/sceneIndexPlugin.cpp:50-54) -- i.e. the ACTUAL order in which
//    UsdImagingCreateSceneIndices appends plugin scene indices
//    (usdImaging/sceneIndices.cpp:68-78, called at :302).
// 2. Builds the real UsdImaging chain and walks it from the terminal via
//    HdFilteringSceneIndexBase::GetInputScenes / GetDisplayName.
// 3. Optionally activates RigExec and dumps a target prim's primvars /
//    extComputationPrimvars to answer "blocked or retained".
// 4. Optionally builds an HdRenderIndex with a display-named null render
//    delegate, inserts the UsdImaging terminal, and walks the render index's
//    terminal scene index to see where renderer-level HdSceneIndexPlugins land.
// 5. Optionally times RigExecImaging_SetTime over a frame range.
//
#include "pxr/pxr.h"

#include "pxr/base/arch/demangle.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/tf/stringUtils.h"

#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/extComputationPrimvarsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"

#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdUtils/stageCache.h"
#include "pxr/usdImaging/usdImaging/sceneIndexPlugin.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// RigExec C surface (declared here so the probe needn't include rigExec
// headers when built without it).
extern "C" int RigExecImaging_Activate(long long, const char *, double);
extern "C" void RigExecImaging_SetTime(double);
extern "C" void RigExecImaging_Deactivate();

namespace {

std::string
_Label(const HdSceneIndexBaseRefPtr &si)
{
    std::string disp = si->GetDisplayName();
    std::string type = ArchGetDemangled(typeid(*get_pointer(si)).name());
    // strip pxrInternal namespace noise
    const size_t p = type.rfind("::");
    if (p != std::string::npos) { type = type.substr(p + 2); }
    if (disp.empty() || disp == type) { return type; }
    return type + " [\"" + disp + "\"]";
}

void
_Walk(const HdSceneIndexBaseRefPtr &si, int depth, int *counter)
{
    if (!si) { return; }
    std::printf("%*s%2d. %s\n", depth * 2, "", (*counter)++, _Label(si).c_str());
    if (HdFilteringSceneIndexBaseRefPtr filtering =
            TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(si)) {
        const std::vector<HdSceneIndexBaseRefPtr> inputs =
            filtering->GetInputScenes();
        for (const HdSceneIndexBaseRefPtr &in : inputs) {
            _Walk(in, depth + (inputs.size() > 1 ? 1 : 0), counter);
        }
    }
}

void
_PrintPluginTypeOrder()
{
    std::set<TfType> pluginTypes;
    PlugRegistry::GetAllDerivedTypes(
        TfType::Find<UsdImagingSceneIndexPlugin>(), &pluginTypes);
    std::printf("--- std::set<TfType> order for UsdImagingSceneIndexPlugin "
                "(%zu types) ---\n", pluginTypes.size());
    int i = 0;
    for (const TfType &t : pluginTypes) {
        // TfType::operator< compares the internal _TypeInfo* (tf/type.h:119,
        // :728). GetTypeid()/name is stable; the ADDRESS is what orders the set.
        std::printf("  [%d] %s\n", i++, t.GetTypeName().c_str());
    }
}

void
_PrintHdPluginOrder()
{
    std::set<TfType> pluginTypes;
    PlugRegistry::GetAllDerivedTypes(
        TfType::Find<HdSceneIndexPlugin>(), &pluginTypes);
    std::printf("--- registered HdSceneIndexPlugin types (%zu) ---\n",
                pluginTypes.size());
    for (const TfType &t : pluginTypes) {
        std::printf("  %s\n", t.GetTypeName().c_str());
    }
}

void
_DumpPrimvars(const HdSceneIndexBaseRefPtr &si, const SdfPath &path,
              const char *stage)
{
    const HdSceneIndexPrim prim = si->GetPrim(path);
    std::printf("--- primvars @ %s (%s) primType=%s ---\n",
                path.GetText(), stage, prim.primType.GetText());
    if (!prim.dataSource) { std::printf("  <no data source>\n"); return; }

    HdContainerDataSourceHandle pvs =
        HdContainerDataSource::Cast(
            prim.dataSource->Get(HdPrimvarsSchemaTokens->primvars));
    if (!pvs) {
        std::printf("  primvars: <absent or not a container>\n");
    } else {
        for (const TfToken &n : pvs->GetNames()) {
            HdDataSourceBaseHandle child = pvs->Get(n);
            const char *kind = "container";
            if (!child) { kind = "NULL"; }
            else if (HdBlockDataSource::Cast(child)) { kind = "BLOCKED"; }
            size_t count = 0;
            if (auto c = HdContainerDataSource::Cast(child)) {
                if (auto v = HdSampledDataSource::Cast(
                        c->Get(HdPrimvarSchemaTokens->primvarValue))) {
                    VtValue val = v->GetValue(0.0);
                    count = val.IsArrayValued() ? val.GetArraySize() : 1;
                }
            }
            std::printf("    primvars:%-24s %-10s n=%zu\n",
                        n.GetText(), kind, count);
        }
    }
    HdContainerDataSourceHandle ecp =
        HdContainerDataSource::Cast(prim.dataSource->Get(
            HdExtComputationPrimvarsSchema::GetSchemaToken()));
    if (!ecp) {
        std::printf("  extComputationPrimvars: <absent>\n");
    } else {
        for (const TfToken &n : ecp->GetNames()) {
            std::printf("    extComputationPrimvars:%s\n", n.GetText());
        }
    }
}


// Reads extComputations/inputValues/<name> off a prim, printing the first
// entries of a Vec3f array. Used to answer "does a dirty on the upstream mesh
// points reach the skinning computation's restPoints input?".
void
_DumpCompInput(const HdSceneIndexBaseRefPtr &si, const SdfPath &path,
               const TfToken &name, double t)
{
    const HdSceneIndexPrim prim = si->GetPrim(path);
    if (!prim.dataSource) {
        std::printf("  t=%.1f  %s : <no prim>\n", t, path.GetText());
        return;
    }
    HdContainerDataSourceHandle c = HdContainerDataSource::Cast(
        prim.dataSource->Get(TfToken("extComputation")));
    if (!c) { std::printf("  t=%.1f  <no extComputation>\n", t); return; }
    HdContainerDataSourceHandle iv = HdContainerDataSource::Cast(
        c->Get(TfToken("inputValues")));
    if (!iv) { std::printf("  t=%.1f  <no inputValues>\n", t); return; }
    HdSampledDataSourceHandle v = HdSampledDataSource::Cast(iv->Get(name));
    if (!v) {
        std::printf("  t=%.1f  <no inputValues:%s> names:", t, name.GetText());
        for (const TfToken &n : iv->GetNames()) std::printf(" %s", n.GetText());
        std::printf("\n");
        return;
    }
    const VtValue val = v->GetValue(0.0);
    if (val.IsHolding<VtVec3fArray>()) {
        const VtVec3fArray a = val.UncheckedGet<VtVec3fArray>();
        std::printf("  t=%.1f  %s n=%zu  [0]=(%.4f,%.4f,%.4f)\n",
                    t, name.GetText(), a.size(), a[0][0], a[0][1], a[0][2]);
    } else {
        std::printf("  t=%.1f  %s type=%s\n", t, name.GetText(),
                    val.GetTypeName().c_str());
    }
}


// Records notices arriving at whatever scene index it is attached to.
class _Recorder : public HdSceneIndexObserver
{
public:
    void PrimsAdded(const HdSceneIndexBase &,
                    const AddedPrimEntries &e) override { added += e.size(); }
    void PrimsRemoved(const HdSceneIndexBase &,
                      const RemovedPrimEntries &e) override
    { removed += e.size(); }
    void PrimsDirtied(const HdSceneIndexBase &,
                      const DirtiedPrimEntries &e) override
    {
        for (const DirtiedPrimEntry &d : e) {
            dirtied.push_back(d.primPath.GetString() + " : " +
                              _LocSetToString(d.dirtyLocators));
        }
    }
    void PrimsRenamed(const HdSceneIndexBase &,
                      const RenamedPrimEntries &) override {}
    static std::string _LocSetToString(const HdDataSourceLocatorSet &s)
    {
        std::string out;
        for (const HdDataSourceLocator &l : s) {
            if (!out.empty()) out += ",";
            out += l.GetString();
        }
        return out.empty() ? "<empty>" : out;
    }
    void Reset() { added = removed = 0; dirtied.clear(); }
    size_t added = 0, removed = 0;
    std::vector<std::string> dirtied;
};

} // namespace

int
main(int argc, char **argv)
{
    const char *usdFile = nullptr;
    const char *dumpPath = nullptr;
    bool doRigExec = false;
    bool doRenderIndex = false;
    bool doBake = false;
    const char *compPath = nullptr;
    bool doNotices = false;
    bool doPull = false;
    int timeFrames = 0;
    double startFrame = 1001.0;
    const char *rendererName = "GL";

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stage") && i + 1 < argc) {
            usdFile = argv[++i];
        } else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) {
            dumpPath = argv[++i];
        } else if (!std::strcmp(argv[i], "--rigexec")) {
            doRigExec = true;
        } else if (!std::strcmp(argv[i], "--pull")) {
            doPull = true;
        } else if (!std::strcmp(argv[i], "--notices")) {
            doNotices = true;
        } else if (!std::strcmp(argv[i], "--comp") && i + 1 < argc) {
            compPath = argv[++i];
        } else if (!std::strcmp(argv[i], "--bake")) {
            doBake = true;
        } else if (!std::strcmp(argv[i], "--renderindex")) {
            doRenderIndex = true;
        } else if (!std::strcmp(argv[i], "--renderer") && i + 1 < argc) {
            rendererName = argv[++i];
        } else if (!std::strcmp(argv[i], "--time") && i + 1 < argc) {
            timeFrames = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--start") && i + 1 < argc) {
            startFrame = std::atof(argv[++i]);
        }
    }

    std::printf("=== PXR_PLUGINPATH_NAME ===\n%s\n\n",
                std::getenv("PXR_PLUGINPATH_NAME")
                    ? std::getenv("PXR_PLUGINPATH_NAME") : "<unset>");

    _PrintPluginTypeOrder();
    std::printf("\n");
    _PrintHdPluginOrder();
    std::printf("\n");

    if (!usdFile) {
        std::printf("no --stage; stopping after type-order dump\n");
        return 0;
    }

    UsdStageRefPtr stage = UsdStage::Open(usdFile);
    if (!stage) { std::printf("FAILED to open %s\n", usdFile); return 2; }

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    const UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    const HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    if (doRigExec) {
        const long long cacheId =
            UsdUtilsStageCache::Get().Insert(stage).ToLongInt();
        const int rc = RigExecImaging_Activate(cacheId, "", startFrame);
        std::printf("RigExecImaging_Activate -> %d\n", rc);
    }

    std::printf("--- UsdImaging chain, terminal -> upstream ---\n");
    int counter = 0;
    _Walk(terminal, 0, &counter);
    std::printf("\n");

    if (dumpPath) {
        _DumpPrimvars(terminal, SdfPath(dumpPath), "usdImaging terminal");
        std::printf("\n");
    }

    if (doNotices && doRigExec) {
        _Recorder rec;
        terminal->AddObserver(HdSceneIndexObserverPtr(&rec));
        for (double t : {1012.0, 1024.0}) {
            rec.Reset();
            RigExecImaging_SetTime(t);
            std::printf("--- notices at terminal for SetTime(%.1f): "
                        "added=%zu removed=%zu dirtied=%zu ---\n",
                        t, rec.added, rec.removed, rec.dirtied.size());
            size_t shown = 0;
            for (const std::string &d : rec.dirtied) {
                std::printf("    %s\n", d.c_str());
                if (++shown >= 12) { std::printf("    ...\n"); break; }
            }
        }
        terminal->RemoveObserver(HdSceneIndexObserverPtr(&rec));
        std::printf("\n");
    }

    if (compPath) {
        std::printf("--- extComputation inputValues at %s ---\n", compPath);
        for (double t : {1001.0, 1012.0, 1024.0}) {
            if (doRigExec) { RigExecImaging_SetTime(t); }
            _DumpCompInput(terminal, SdfPath(compPath),
                           TfToken("restPoints"), t);
        }
        std::printf("\n");
    }

    if (doBake) {
        // HdSiExtComputationPrimvarPruningSceneIndex turns computed primvars
        // back into ordinary authored primvars, running the CPU kernel when
        // the value is pulled (hdsi/extComputationPrimvarPruningSceneIndex.h
        // :19-33). This is the mechanism a downstream generator can use to see
        // the SKINNED points.
        HdSceneIndexBaseRefPtr baked =
            HdSiExtComputationPrimvarPruningSceneIndex::New(terminal);
        baked->SetDisplayName("bake(ExtComputationPrimvarPruning)");
        std::printf("--- baked chain ---\n");
        counter = 0;
        _Walk(baked, 0, &counter);
        std::printf("\n");
        if (dumpPath) {
            _DumpPrimvars(baked, SdfPath(dumpPath), "baked terminal");
            if (doRigExec) {
                for (double t : {1001.0, 1012.0, 1024.0}) {
                    RigExecImaging_SetTime(t);
                    const HdSceneIndexPrim prim = baked->GetPrim(SdfPath(dumpPath));
                    HdPrimvarsSchema pvs =
                        HdPrimvarsSchema::GetFromParent(prim.dataSource);
                    HdPrimvarSchema pv = pvs.GetPrimvar(HdTokens->points);
                    VtValue v = pv.GetPrimvarValue()
                        ? pv.GetPrimvarValue()->GetValue(0.0) : VtValue();
                    if (v.IsHolding<VtVec3fArray>()) {
                        const VtVec3fArray a = v.UncheckedGet<VtVec3fArray>();
                        std::printf("  t=%.1f  n=%zu  p[0]=(%.4f,%.4f,%.4f) "
                                    "p[5]=(%.4f,%.4f,%.4f)\n",
                                    t, a.size(),
                                    a[0][0], a[0][1], a[0][2],
                                    a[5][0], a[5][1], a[5][2]);
                    } else {
                        std::printf("  t=%.1f  points NOT AVAILABLE (%s)\n",
                                    t, v.GetTypeName().c_str());
                    }
                }
            }
            std::printf("\n");
        }
    }

    if (doRenderIndex) {
        // The renderer display name can only be set by HdRendererPlugin
        // (hd/renderDelegate.h:584-589, hd/rendererPlugin.cpp:76-78), and
        // hd/renderIndex.cpp:208 only calls AppendSceneIndicesForRenderer when
        // it is non-empty. So go through the plugin registry, exactly like a
        // real application.
        HdRendererPluginHandle plugin =
            HdRendererPluginRegistry::GetInstance()
                .GetOrCreateRendererPlugin(TfToken(rendererName));
        if (!plugin) {
            std::printf("no renderer plugin \"%s\"; available:\n",
                        rendererName);
            HfPluginDescVector descs;
            HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
            for (const HfPluginDesc &d : descs) {
                std::printf("    id=%s displayName=\"%s\"\n",
                            d.id.GetText(), d.displayName.c_str());
            }
        } else {
            HdPluginRenderDelegateUniqueHandle rd = plugin->CreateDelegate();
            std::printf("render delegate display name = \"%s\"\n",
                        rd ? rd->GetRendererDisplayName().c_str() : "<null>");
            // A directly-constructed delegate for contrast is impossible to
            // build here: _SetRendererDisplayName is private.
            HdRenderIndex *ri = HdRenderIndex::New(
                rd.Get(), HdDriverVector(), "G_chain_order_probe", "");
            if (!ri) {
                std::printf("HdRenderIndex::New returned null\n");
            } else {
                ri->InsertSceneIndex(terminal, SdfPath::AbsoluteRootPath());
                std::printf("--- HdRenderIndex terminal chain (renderer=\"%s\")"
                            " ---\n", rendererName);
                counter = 0;
                _Walk(ri->GetTerminalSceneIndex(), 0, &counter);
                std::printf("\n");
                if (dumpPath) {
                    _DumpPrimvars(ri->GetTerminalSceneIndex(),
                                  SdfPath(dumpPath), "render index terminal");
                    std::printf("\n");
                }
                delete ri;
            }
        }
    }

    if (timeFrames > 0 && doRigExec) {
        std::printf("--- RigExecImaging_SetTime timing, %d frames from %.1f "
                    "---\n", timeFrames, startFrame);
        // warm up
        RigExecImaging_SetTime(startFrame);
        double worst = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < timeFrames; ++f) {
            const auto a = std::chrono::steady_clock::now();
            RigExecImaging_SetTime(startFrame + f);
            const auto b = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(b - a).count();
            if (ms > worst) { worst = ms; }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double total =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("  total=%.3f ms  mean=%.4f ms/frame  worst=%.4f ms\n",
                    total, total / timeFrames, worst);
    }

    if (doPull) {
        // Full traversal + points pull: what a downstream curve generator
        // does when it has to re-read every surface it is bound to.
        std::function<void(const SdfPath &, size_t *, size_t *)> visit =
            [&](const SdfPath &p, size_t *prims, size_t *pts) {
                ++(*prims);
                const HdSceneIndexPrim prim = terminal->GetPrim(p);
                if (prim.dataSource) {
                    HdPrimvarsSchema pvs =
                        HdPrimvarsSchema::GetFromParent(prim.dataSource);
                    HdPrimvarSchema pv = pvs.GetPrimvar(HdTokens->points);
                    if (pv.GetPrimvarValue()) {
                        const VtValue v = pv.GetPrimvarValue()->GetValue(0.0);
                        if (v.IsHolding<VtVec3fArray>()) {
                            *pts += v.UncheckedGet<VtVec3fArray>().size();
                        }
                    }
                }
                for (const SdfPath &c : terminal->GetChildPrimPaths(p)) {
                    visit(c, prims, pts);
                }
            };
        size_t prims = 0, pts = 0;
        visit(SdfPath::AbsoluteRootPath(), &prims, &pts);   // warm
        const int reps = timeFrames > 0 ? timeFrames : 48;
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < reps; ++f) {
            if (doRigExec) { RigExecImaging_SetTime(startFrame + f); }
            prims = pts = 0;
            visit(SdfPath::AbsoluteRootPath(), &prims, &pts);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double total =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("--- SetTime + full traversal pull, %d reps ---\n", reps);
        std::printf("  prims=%zu pointsRead=%zu  total=%.3f ms  "
                    "mean=%.4f ms/frame\n", prims, pts, total, total / reps);
    }

    if (doRigExec) { RigExecImaging_Deactivate(); }
    std::printf("probeChainOrder: DONE\n");
    return 0;
}
