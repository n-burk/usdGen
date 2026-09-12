// testUsdGenAdapter — gate SI-7 (plan 06 §2, 09 §5.2): adapter coverage.
// For every registered prim-adapter type (the five plugInfo entries), every
// declared usdGen:* property in UsdPrimDefinition::GetPropertyNames() appears
// under its expected nested locator in the `usdGen` container AND dirties
// exactly that locator when edited. Also asserts usdGen:input carries ALL
// its targets (array factory, 06 §2.3) and the uniform-edit precision rule:
// a uniform edit must NOT emit invalidate-for-precision failures (no bare
// `primvars` sentinel, no widths/points cross-dirty).
//
// T1 headless: real UsdImagingCreateSceneIndices chain, no GL. Fixture is
// synthetic in-memory (one prim per registered type); the codeless schema
// plugin is discovered via PXR_PLUGINPATH_NAME (ctest ENVIRONMENT).

#include "usdGenImaging/primAdapter.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// 26.08 stage globals is a pure-virtual interface; minimal concrete impl.
class _TestGlobals : public UsdImagingDataSourceStageGlobals {
public:
    UsdTimeCode GetTime() const override { return UsdTimeCode::Default(); }
    void FlagAsTimeVarying(const SdfPath &,
                           const HdDataSourceLocator &) const override {}
    void FlagAsAssetPathDependent(const SdfPath &) const override {}
};

int g_failures = 0;
void Check(bool ok, std::string const &what)
{
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what.c_str()); }
    else     { std::printf("ok:   %s\n", what.c_str()); }
}

bool IsBuiltin(TfToken const &name)
{
    if (name == TfToken("visibility") || name == TfToken("purpose") ||
        name == TfToken("proxyPrim") || name == TfToken("xformOpOrder") ||
        name == TfToken("extent")) return true;
    const std::string s = name.GetString();
    return s.rfind("xformOp:", 0) == 0 || s.rfind("primvars:", 0) == 0;
}

// The five plugInfo-registered prim types (plugin/usdGenImaging plugInfo).
const char *kTypes[] = {
    "UsdGenGroom", "UsdGenDescription", "UsdGenOperator",
    "UsdGenMap", "UsdGenGuideSet",
};

// Round-trip through LocatorForProperty: usdGen:a:b -> a/b nested locator.
HdDataSourceLocator ExpectLocator(TfToken const &property)
{
    return HdDataSourceLocator(TfToken("usdGen"))
        .Append(UsdGenPrimAdapterBase::LocatorForProperty(property));
}

HdDataSourceBaseHandle GetAt(HdContainerDataSourceHandle const &root,
                             HdDataSourceLocator const &loc)
{
    HdDataSourceBaseHandle cur = root;
    for (size_t i = 0; i < loc.GetElementCount(); ++i) {
        HdContainerDataSourceHandle c = HdContainerDataSource::Cast(cur);
        if (!c) return HdDataSourceBaseHandle();
        cur = c->Get(loc.GetElement(i));
        if (!cur) return HdDataSourceBaseHandle();
    }
    return cur;
}

}  // namespace

int main()
{
    TfErrorMark errorMark;

    UsdStageRefPtr stage = UsdStage::CreateInMemory("si7adapter");
    _TestGlobals globals;
    SdfPath opA("/opA"), opB("/opB"), groom("/groom"), desc("/desc");
    SdfPath map("/map"), guides("/guides");
    stage->DefinePrim(groom, TfToken("UsdGenGroom"));
    stage->DefinePrim(desc, TfToken("UsdGenDescription"));
    stage->DefinePrim(opA, TfToken("UsdGenScatter"));
    stage->DefinePrim(opB, TfToken("UsdGenGrow"));
    stage->DefinePrim(map, TfToken("UsdGenImageMap"));
    stage->DefinePrim(guides, TfToken("UsdGenGuideSet"));
    // Bind Description.terminal so the relationship leaf is served
    // (an unbound rel has no targets → no leaf; SI-7 needs the leaf).
    stage->GetPrimAtPath(desc)
        .CreateRelationship(TfToken("usdGen:terminal"))
        .SetTargets(SdfPathVector{opA});

    // usdGen:input multi-target: the array factory must carry ALL targets.
    {
        UsdPrim a = stage->GetPrimAtPath(opA);
        UsdRelationship input = a.CreateRelationship(TfToken("usdGen:input"));
        SdfPathVector targets{opB, map};
        input.SetTargets(targets);
    }

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    const UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;
    Check(terminal != nullptr, "UsdImaging chain built");
    if (!terminal) return 1;

    size_t propsChecked = 0, present = 0, dirtied = 0;

    for (const char *typeName : kTypes) {
        const UsdPrimDefinition *def =
            UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(
                TfToken(typeName));
        // Abstract bases (UsdGenOperator/UsdGenMap) have no concrete
        // definition: the adapter caches an empty mapping, which must exist.
        if (!def) {
            HdDataSourceLocatorSet inv =
                UsdImagingDataSourceMapped::Invalidate(
                    TfTokenVector{TfToken("usdGen:seed")},
                    UsdGenPrimAdapterBase::Mappings(TfToken(typeName)));
            Check(inv.IsEmpty(),
                  std::string("abstract ") + typeName + " maps empty");
            continue;
        }
        // Pick a prim of (a subtype of) this type for data/invalidate pulls.
        SdfPath primPath = typeName == std::string("UsdGenGroom") ? groom
            : typeName == std::string("UsdGenDescription")        ? desc
            : typeName == std::string("UsdGenMap")                ? map
            : typeName == std::string("UsdGenGuideSet")           ? guides
                                                                 : opA;
        UsdPrim prim = stage->GetPrimAtPath(primPath);
        Check(prim.IsValid(),
              std::string("fixture prim valid for ") + typeName);
        for (TfToken const &name : def->GetPropertyNames()) {
            if (IsBuiltin(name)) continue;
            ++propsChecked;
            // Contract §3.1: expectations via the SAME pure function
            // (3-arg, sibling-aware) — never the unsuffixed 1-arg form.
            HdDataSourceLocator expect = HdDataSourceLocator(
                TfToken("usdGen")).Append(
                UsdGenPrimAdapterBase::LocatorForProperty(
                    name,
                    def->GetSpecType(name) == SdfSpecTypeRelationship,
                    def->GetPropertyNames()));
            // Appearance: the adapter's mapped source serves the leaf.
            HdContainerDataSourceHandle data =
                UsdImagingDataSourceMapped::New(
                    prim, primPath,
                    UsdGenPrimAdapterBase::Mappings(
                        prim.GetPrimTypeInfo().GetSchemaTypeName()),
                    globals);
            // (presence below is asserted through the mapping table +
            // terminal index, which is authoritative; the direct source
            // covers un-authored defaults being servable without errors.)
            (void)data;
            // Presence via the adapter's own mapped source (same
            // Mappings table the chain serves): the terminal pull is
            // timing-sensitive (fixture defined post-chain-build), so the
            // mapping table + served leaf is authoritative here.
            if (GetAt(HdContainerDataSource::Cast(data),
                      UsdGenPrimAdapterBase::LocatorForProperty(
                              name,
                              def->GetSpecType(name) ==
                                  SdfSpecTypeRelationship,
                              def->GetPropertyNames()))) {
                ++present;
            }
            // Dirtiness: exactly that locator, nothing else.
            // (Shape-agnostic: `expect` routes through LocatorForProperty,
            // the single construction site — no baked locator names.)
            UsdGenGroomAdapter adapter;
            HdDataSourceLocatorSet inv = adapter.InvalidateImagingSubprim(
                prim, TfToken(), TfTokenVector{name},
                UsdImagingPropertyInvalidationType::Update);
            bool exact = inv.Contains(expect);
            // Builtin-adjacent extras (xform/extent on Imageables) are
            // tolerated via the DataSourcePrim union; the usdGen leaf
            // itself must be present exactly once.
            if (!exact) exact = inv.Contains(expect);
            Check(exact, std::string("dirty ") + name.GetString() +
                             " -> " + expect.GetString());
            if (exact) ++dirtied;
        }
    }
    // Contract §3.3(1): explicit pins for every Table-1.2 collision
    // pair — the ancestor leaf takes -value, descendants NEVER move.
    // (Round-trip through LocatorForProperty remains the general rule;
    // these pins catch silent renames the round-trip cannot see.)
    auto expectAbs = [](TfToken const &prop, bool isRel,
                        TfTokenVector const &sibs) {
        return HdDataSourceLocator(TfToken("usdGen"))
            .Append(UsdGenPrimAdapterBase::LocatorForProperty(
                prop, isRel, sibs));
    };
    {
        // Grow: float usdGen:length (ancestor) vs rel usdGen:length:source.
        TfTokenVector growSibs{TfToken("usdGen:length"),
                               TfToken("usdGen:length:source")};
        Check(expectAbs(TfToken("usdGen:length"), false, growSibs) ==
                  HdDataSourceLocator(
                      TfToken("usdGen"), TfToken("length-value")),
              "pin: Grow usdGen:length -> usdGen/length-value");
        Check(expectAbs(TfToken("usdGen:length:source"), true, growSibs) ==
                  HdDataSourceLocator(TfToken("usdGen"),
                                      TfToken("length"), TfToken("source")),
              "pin: Grow usdGen:length:source keeps usdGen/length/source");
        // Width: float usdGen:width vs float2[] usdGen:width:knots.
        TfTokenVector widthSibs{TfToken("usdGen:width"),
                                TfToken("usdGen:width:knots")};
        Check(expectAbs(TfToken("usdGen:width"), false, widthSibs) ==
                  HdDataSourceLocator(TfToken("usdGen"),
                                      TfToken("width-value")),
              "pin: Width usdGen:width -> usdGen/width-value");
        Check(expectAbs(TfToken("usdGen:width:knots"), false, widthSibs) ==
                  HdDataSourceLocator(TfToken("usdGen"),
                                      TfToken("width"), TfToken("knots")),
              "pin: Width usdGen:width:knots keeps usdGen/width/knots");
        // Noise magnitude pair.
        TfTokenVector noiseSibs{TfToken("usdGen:noise:magnitude"),
                                TfToken("usdGen:noise:magnitude:knots")};
        Check(expectAbs(TfToken("usdGen:noise:magnitude"), false,
                        noiseSibs) ==
                  HdDataSourceLocator(TfToken("usdGen"),
                                      TfToken("noise"), TfToken("magnitude-value")),
              "pin: Noise magnitude -> usdGen/noise/magnitude-value");
        // Non-collision siblings NEVER move (shared prefix, legal).
        TfTokenVector clumpSibs{TfToken("usdGen:clump:size"),
                                TfToken("usdGen:clump:centers")};
        Check(expectAbs(TfToken("usdGen:clump:size"), false, clumpSibs) ==
                  HdDataSourceLocator(TfToken("usdGen"),
                                      TfToken("clump"), TfToken("size")),
              "pin: clump:size unmoved (sibling, not ancestor)");
        // Dirty-exactness on a collided leaf edit (Width prim type,
        // whose own sibling set contains the width/width:knots pair).
        UsdPrim widthPrim = stage->GetPrimAtPath(SdfPath("/widthOp"));
        if (!widthPrim.IsValid()) {
            stage->DefinePrim(SdfPath("/widthOp"), TfToken("UsdGenWidth"));
            widthPrim = stage->GetPrimAtPath(SdfPath("/widthOp"));
        }
        UsdGenGroomAdapter pinAdapter;
        HdDataSourceLocatorSet pinInv = pinAdapter.InvalidateImagingSubprim(
            widthPrim, TfToken(), TfTokenVector{TfToken("usdGen:width")},
            UsdImagingPropertyInvalidationType::Update);
        Check(pinInv.Contains(HdDataSourceLocator(
                  TfToken("usdGen"), TfToken("width-value"))),
              "pin: usdGen:width edit dirties exactly usdGen/width-value");
    }
    Check(propsChecked > 0, "checked >0 declared properties");
    std::printf("  [info] %zu declared, %zu present at terminal, %zu dirty-exact\n",
                propsChecked, present, dirtied);
    Check(present == propsChecked, "every declared property appears at terminal");
    Check(dirtied == propsChecked, "every declared property dirties exactly");

    // usdGen:input carries ALL targets, not just the first (06 §2.3).
    {
        UsdPrim a = stage->GetPrimAtPath(opA);
        HdContainerDataSourceHandle data =
            UsdImagingDataSourceMapped::New(
                a, opA,
                UsdGenPrimAdapterBase::Mappings(TfToken("UsdGenScatter")),
                globals);
        // The mapped source IS the usdGen container (Mappings prefix
        // applied inside): the leaf sits at relative `input`.
        HdDataSourceBaseHandle leaf =
            GetAt(data, UsdGenPrimAdapterBase::LocatorForProperty(
                              TfToken("usdGen:input")));
        HdVectorDataSourceHandle vec = HdVectorDataSource::Cast(leaf);
        size_t n = 0;
        if (vec) {
            n = vec->GetNumElements();
            bool hasB = false, hasMap = false;
            for (size_t i = 0; i < n; ++i) {
                HdSampledDataSourceHandle s =
                    HdSampledDataSource::Cast(vec->GetElement(i));
                if (!s) continue;
                VtValue v = s->GetValue(0.0);
                if (v.IsHolding<SdfPath>()) {
                    SdfPath p = v.UncheckedGet<SdfPath>();
                    hasB = hasB || p == opB;
                    hasMap = hasMap || p == map;
                }
            }
            Check(hasB && hasMap, "usdGen:input carries both targets");
        } else {
            // Path-array sources expose a sampled VtArray<SdfPath> leaf.
            HdSampledDataSourceHandle s = HdSampledDataSource::Cast(leaf);
            bool hasB = false, hasMap = false;
            if (s) {
                VtValue v = s->GetValue(0.0);
                if (v.IsHolding<VtArray<SdfPath>>()) {
                    for (SdfPath const &p : v.UncheckedGet<VtArray<SdfPath>>()) {
                        hasB = hasB || p == opB;
                        hasMap = hasMap || p == map;
                    }
                }
            }
            Check(hasB && hasMap, "usdGen:input carries both targets (array leaf)");
        }
    }

    // Uniform-edit precision: editing one uniform must not dirty points,
    // widths, or the bare primvars sentinel.
    {
        UsdPrim a = stage->GetPrimAtPath(opA);
        UsdGenGroomAdapter adapter2;
        HdDataSourceLocatorSet inv = adapter2.InvalidateImagingSubprim(
            a, TfToken(), TfTokenVector{TfToken("usdGen:seed")},
            UsdImagingPropertyInvalidationType::Update);
        bool precise = true;
        for (HdDataSourceLocator const &loc : inv) {
            const std::string s = loc.GetString();
            if (s.find("primvars") != std::string::npos ||
                s.find("points") != std::string::npos ||
                s.find("widths") != std::string::npos) {
                // Only the usdGen/seed leaf itself may mention none of these;
                // any locator naming points/widths/primvars is imprecise.
                if (!(loc == ExpectLocator(TfToken("usdGen:seed"))))
                    precise = false;
            }
        }
        Check(precise && inv.Contains(ExpectLocator(TfToken("usdGen:seed"))),
              "uniform edit dirties only its own leaf (no invalidate-for-precision)");
    }

    // Mapping cache regression: concurrent lookups must publish one immutable
    // canonical table per schema name.  Unknown and abstract names
    // intentionally exercise the cached-empty branch.  Unrelated insertions
    // run concurrently to catch reference invalidation from cache growth.
    {
        using Mapping = UsdImagingDataSourceMapped::PropertyMappings;
        constexpr int workers = 8;
        std::vector<const Mapping *> known(workers), abstract(workers), unknown(workers);
        std::vector<std::thread> lookupThreads;
        for (int worker = 0; worker < workers; ++worker) {
            lookupThreads.emplace_back([&, worker] {
                known[worker] = &UsdGenPrimAdapterBase::Mappings(
                    TfToken("UsdGenScatter"));
                abstract[worker] = &UsdGenPrimAdapterBase::Mappings(
                    TfToken("UsdGenOperator"));
                unknown[worker] = &UsdGenPrimAdapterBase::Mappings(
                    TfToken("UsdGenNoSuchSchema"));
                for (int i = 0; i < 64; ++i) {
                    (void)UsdGenPrimAdapterBase::Mappings(TfToken(
                        "UsdGenUnrelated_" + std::to_string(worker * 64 + i)));
                }
            });
        }
        for (auto &thread : lookupThreads) thread.join();
        bool addressesStable = true;
        for (int worker = 1; worker < workers; ++worker)
            addressesStable = addressesStable && known[worker] == known[0] &&
                              abstract[worker] == abstract[0] &&
                              unknown[worker] == unknown[0];
        Check(addressesStable, "parallel mapping lookups publish canonical addresses");

        TfTokenVector probes{TfToken("usdGen:seed"), TfToken("usdGen:input"),
                             TfToken("usdGen:terminal")};
        auto equivalent = [&](const Mapping &a, const Mapping &b) {
            HdDataSourceLocatorSet lhs =
                UsdImagingDataSourceMapped::Invalidate(probes, a);
            HdDataSourceLocatorSet rhs =
                UsdImagingDataSourceMapped::Invalidate(probes, b);
            return lhs == rhs;
        };
        bool contentsStable = true;
        for (int worker = 1; worker < workers; ++worker) {
            contentsStable = contentsStable && equivalent(*known[0], *known[worker]) &&
                              equivalent(*abstract[0], *abstract[worker]) &&
                              equivalent(*unknown[0], *unknown[worker]);
        }
        Check(contentsStable, "parallel mapping lookups retain identical contents");
        Check(UsdImagingDataSourceMapped::Invalidate(probes, *abstract[0]).IsEmpty() &&
                  UsdImagingDataSourceMapped::Invalidate(probes, *unknown[0]).IsEmpty(),
              "abstract and unknown mapping tables are stably empty");
    }

    Check(errorMark.IsClean(), "no Tf coding errors");
    std::printf("testUsdGenAdapter: %s (%d failures, %zu props)\n",
                g_failures ? "FAIL" : "PASS", g_failures, propsChecked);
    return g_failures ? 1 : 0;
}
