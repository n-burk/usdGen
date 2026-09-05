// Probe: stage-free parameter and time transport for a Hydra-2.0 grooming plugin.
// Answers:
//  (1) What does the terminal UsdImaging scene index expose for a prim of an
//      unknown *codeless* schema type with no imaging adapter?
//  (2) What do UsdImagingDataSourceAttributeNew / DataSourceRelationship /
//      DataSourceMapped yield for asset, asset[], float2[], float[], token,
//      string, rel and Ts-spline-valued attributes?
//  (3) Is a resolved asset path available stage-free? Is a UDIM identifier?
//  (4) Is a Ts spline visible as a curve, or only as a time-sampled scalar?
//  (5) Can a data source read a REST value (UsdTimeCode::Default()) while the
//      stage globals time is numeric?  (custom AttributeMapping::factory)
//  (6) Does time-varying flagging fire for splines (=> per-frame dirtying)?
#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/attributeQuery.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/primDefinition.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usdGeom/imageable.h"

#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/primOriginSchema.h"

#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include "pxr/usdImaging/usdImaging/dataSourceAttribute.h"
#include "pxr/usdImaging/usdImaging/dataSourceRelationship.h"
#include "pxr/usdImaging/usdImaging/dataSourceMapped.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"

#include <iostream>
#include <iomanip>
#include <set>
#include <map>

PXR_NAMESPACE_USING_DIRECTIVE

static std::string _Hdr(const std::string &s) {
    return "\n===== " + s + " =====";
}

// ---------------------------------------------------------------- value print
static std::string _Val(const VtValue &v)
{
    std::string t = v.GetTypeName();
    if (v.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath &a = v.UncheckedGet<SdfAssetPath>();
        return "SdfAssetPath{path='" + a.GetAssetPath() +
               "', resolved='" + a.GetResolvedPath() + "'}";
    }
    if (v.IsHolding<VtArray<SdfAssetPath>>()) {
        std::string r = "SdfAssetPath[]{";
        for (const SdfAssetPath &a : v.UncheckedGet<VtArray<SdfAssetPath>>()) {
            r += "('" + a.GetAssetPath() + "','" + a.GetResolvedPath() + "') ";
        }
        return r + "}";
    }
    return "(" + t + ") " + TfStringify(v);
}

// -------------------------------------------------------------- ds recursion
static void _Dump(const HdDataSourceBaseHandle &ds, const std::string &indent,
                  int depth)
{
    if (!ds) { std::cout << indent << "<null>\n"; return; }
    if (depth <= 0) { std::cout << indent << "...\n"; return; }
    if (auto c = HdContainerDataSource::Cast(ds)) {
        for (const TfToken &n : c->GetNames()) {
            HdDataSourceBaseHandle child = c->Get(n);
            std::string kind = "?";
            if (HdContainerDataSource::Cast(child)) kind = "container";
            else if (HdVectorDataSource::Cast(child)) kind = "vector";
            else if (HdSampledDataSource::Cast(child)) kind = "sampled";
            else if (!child) kind = "null";
            std::cout << indent << n.GetString() << "  [" << kind << "]";
            if (auto s = HdSampledDataSource::Cast(child)) {
                std::cout << " = " << _Val(s->GetValue(0.0));
                std::vector<HdSampledDataSource::Time> ts;
                if (s->GetContributingSampleTimesForInterval(-0.25, 0.25, &ts)) {
                    std::cout << "  contribTimes{";
                    for (auto t : ts) std::cout << t << " ";
                    std::cout << "}";
                }
            }
            std::cout << "\n";
            if (HdContainerDataSource::Cast(child) ||
                HdVectorDataSource::Cast(child)) {
                _Dump(child, indent + "  ", depth - 1);
            }
        }
        return;
    }
    if (auto v = HdVectorDataSource::Cast(ds)) {
        for (size_t i = 0; i < v->GetNumElements(); ++i) {
            std::cout << indent << "[" << i << "]\n";
            _Dump(v->GetElement(i), indent + "  ", depth - 1);
        }
        return;
    }
    if (auto s = HdSampledDataSource::Cast(ds)) {
        std::cout << indent << "= " << _Val(s->GetValue(0.0)) << "\n";
        return;
    }
    std::cout << indent << "<opaque>\n";
}

static void _DumpScene(const HdSceneIndexBaseRefPtr &si, const SdfPath &p,
                       int depth)
{
    HdSceneIndexPrim prim = si->GetPrim(p);
    std::cout << "\n-- " << p << "  primType='" << prim.primType.GetString()
              << "'\n";
    if (prim.dataSource) {
        std::cout << "   names:";
        for (const TfToken &n : prim.dataSource->GetNames())
            std::cout << " " << n;
        std::cout << "\n";
    } else {
        std::cout << "   dataSource = null\n";
    }
    for (const SdfPath &c : si->GetChildPrimPaths(p)) {
        _DumpScene(si, c, depth - 1);
    }
}

// ------------------------------------------------------------- stage globals
class ProbeStageGlobals : public UsdImagingDataSourceStageGlobals
{
public:
    explicit ProbeStageGlobals(UsdTimeCode t) : _time(t) {}
    UsdTimeCode GetTime() const override { return _time; }
    void SetTime(UsdTimeCode t) { _time = t; }
    void FlagAsTimeVarying(const SdfPath &p,
                           const HdDataSourceLocator &l) const override {
        _tv[p].insert(l);
    }
    void FlagAsAssetPathDependent(const SdfPath &p) const override {
        _ap.insert(p);
    }
    void Report() const {
        std::cout << "  FlagAsTimeVarying:\n";
        for (auto &e : _tv) {
            std::cout << "    " << e.first << " -> ";
            for (auto &l : e.second) std::cout << "'" << l.GetString() << "' ";
            std::cout << "\n";
        }
        std::cout << "  FlagAsAssetPathDependent:\n";
        for (auto &p : _ap) std::cout << "    " << p << "\n";
    }
    void Clear() const { _tv.clear(); _ap.clear(); }
private:
    UsdTimeCode _time;
    mutable std::map<SdfPath, HdDataSourceLocatorSet> _tv;
    mutable std::set<SdfPath> _ap;
};

// ------------------------------------- REST-time (UsdTimeCode::Default()) DS
template <typename T>
class RestTimeAttrDataSource : public HdTypedSampledDataSource<T>
{
public:
    using Handle = std::shared_ptr<RestTimeAttrDataSource<T>>;
    static Handle New(const UsdAttribute &a) {
        return Handle(new RestTimeAttrDataSource<T>(a));
    }
    VtValue GetValue(HdSampledDataSource::Time t) override {
        return VtValue(GetTypedValue(t));
    }
    T GetTypedValue(HdSampledDataSource::Time) override {
        T r{};
        _q.Get<T>(&r, UsdTimeCode::Default());
        return r;
    }
    bool GetContributingSampleTimesForInterval(
        HdSampledDataSource::Time, HdSampledDataSource::Time,
        std::vector<HdSampledDataSource::Time> *) override { return false; }
private:
    explicit RestTimeAttrDataSource(const UsdAttribute &a) : _q(a) {}
    UsdAttributeQuery _q;
};

static HdSampledDataSourceHandle
_RestPointsFactory(const UsdAttribute &attr,
                   const UsdImagingDataSourceStageGlobals &,
                   const SdfPath &, const HdDataSourceLocator &)
{
    return RestTimeAttrDataSource<VtVec3fArray>::New(attr);
}

int main(int argc, char **argv)
{
    const std::string sceneFile = (argc > 1) ? argv[1] : "scene.usda";
    UsdStageRefPtr stage = UsdStage::Open(sceneFile);
    if (!stage) { std::cerr << "cannot open " << sceneFile << "\n"; return 1; }

    // --------------------------------------------------------------- part 0
    std::cout << _Hdr("0. codeless schema registration") << "\n";
    UsdPrim clump = stage->GetPrimAtPath(SdfPath("/World/Clump"));
    std::cout << "prim /World/Clump typeName = "
              << clump.GetTypeName() << "\n"
              << "GetPrimTypeInfo().GetSchemaTypeName() = "
              << clump.GetPrimTypeInfo().GetSchemaTypeName() << "\n"
              << "IsA(\"UsdGenProbeOperator\") = "
              << clump.IsA(TfToken("UsdGenProbeOperator")) << "\n"
              << "IsA UsdGeomImageable = " << clump.IsA<UsdGeomImageable>()
              << "\n";
    {
        const UsdPrimDefinition &def = clump.GetPrimDefinition();
        std::cout << "UsdPrimDefinition::GetPropertyNames():\n";
        for (const TfToken &n : def.GetPropertyNames()) {
            SdfSpecType st = def.GetSpecType(n);
            std::string ty;
            if (st == SdfSpecTypeAttribute) {
                if (SdfAttributeSpecHandle sp = def.GetSchemaAttributeSpec(n)) {
                    ty = sp->GetTypeName().GetAsToken().GetString();
                }
            }
            std::cout << "   " << n << "  spec="
                      << TfEnum::GetName(st) << " type=" << ty << "\n";
        }
    }

    // --------------------------------------------------------------- part 1
    std::cout << _Hdr("1. terminal scene index of UsdImagingCreateSceneIndices");
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;
    _DumpScene(terminal, SdfPath::AbsoluteRootPath(), 10);

    std::cout << _Hdr("1b. full data source of /World/Clump (codeless, no adapter)");
    _Dump(terminal->GetPrim(SdfPath("/World/Clump")).dataSource, "  ", 6);

    std::cout << _Hdr("1c. primOrigin of /World/Clump");
    {
        HdSceneIndexPrim p = terminal->GetPrim(SdfPath("/World/Clump"));
        if (auto po = HdPrimOriginSchema::GetFromParent(p.dataSource)) {
            std::cout << "  primOrigin names:";
            for (const TfToken &n : po.GetContainer()->GetNames())
                std::cout << " " << n;
            std::cout << "\n";
            for (const TfToken &n : po.GetContainer()->GetNames()) {
                auto ds = po.GetContainer()->Get(n);
                std::cout << "   " << n << " -> "
                          << (HdSampledDataSource::Cast(ds)
                                ? _Val(HdSampledDataSource::Cast(ds)->GetValue(0))
                                : std::string("<not sampled>")) << "\n";
            }
        } else {
            std::cout << "  <no primOrigin>\n";
        }
    }

    std::cout << _Hdr("1d. /World/ImageableOp (codeless, Imageable base)");
    _Dump(terminal->GetPrim(SdfPath("/World/ImageableOp")).dataSource, "  ", 4);

    std::cout << _Hdr("1e. /World/Scalp mesh points @time1 then @time24");
    auto pointsOf = [&](const char *t) {
        HdSceneIndexPrim p = terminal->GetPrim(SdfPath("/World/Scalp"));
        auto c = HdContainerDataSource::Cast(p.dataSource->Get(TfToken("primvars")));
        auto pv = HdContainerDataSource::Cast(c->Get(TfToken("points")));
        auto v = HdSampledDataSource::Cast(pv->Get(TfToken("primvarValue")));
        std::cout << "  " << t << " points = " << _Val(v->GetValue(0.0)) << "\n";
        std::vector<HdSampledDataSource::Time> ts;
        if (v->GetContributingSampleTimesForInterval(-0.25, 0.25, &ts)) {
            std::cout << "    contribTimes:";
            for (auto x : ts) std::cout << " " << x;
            std::cout << "\n";
        }
    };
    pointsOf("t=1");
    sis.stageSceneIndex->SetTime(UsdTimeCode(24.0));
    pointsOf("t=24");
    {
        HdSceneIndexPrim p = terminal->GetPrim(SdfPath("/World/Scalp"));
        auto c = HdContainerDataSource::Cast(p.dataSource->Get(TfToken("primvars")));
        std::cout << "  primvars on Scalp:";
        for (const TfToken &n : c->GetNames()) std::cout << " " << n;
        std::cout << "\n";
        auto rest = HdContainerDataSource::Cast(c->Get(TfToken("rest")));
        if (rest) {
            auto v = HdSampledDataSource::Cast(rest->Get(TfToken("primvarValue")));
            std::cout << "  primvars:rest @t=24 = " << _Val(v->GetValue(0.0)) << "\n";
        }
    }
    sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));

    // --------------------------------------------------------------- part 2
    std::cout << _Hdr("2. direct data sources (UsdImagingDataSourceAttributeNew)");
    ProbeStageGlobals g(UsdTimeCode(1.0));
    const SdfPath sip("/World/Clump");
    struct Case { const char *name; };
    const char *attrs[] = {
        "usdGen:map", "usdGen:mapArray", "usdGen:ramp:knots",
        "usdGen:ramp:values", "usdGen:clumpRadius", "usdGen:frizzAmount",
        "usdGen:seed", "usdGen:expression", "usdGen:mode", "usdGen:density"
    };
    for (const char *an : attrs) {
        UsdAttribute a = clump.GetAttribute(TfToken(an));
        if (!a) { std::cout << "  " << an << " : <no attribute>\n"; continue; }
        HdSampledDataSourceHandle ds = UsdImagingDataSourceAttributeNew(
            a, g, sip, HdDataSourceLocator(TfToken("usdGen"), TfToken(an)));
        std::cout << "  " << an << " (" << a.GetTypeName().GetAsToken()
                  << ")  authored=" << a.IsAuthored()
                  << "  hasSpline=" << a.HasSpline()
                  << "  mightVary=" << a.ValueMightBeTimeVarying() << "\n";
        if (!ds) { std::cout << "    <null data source>\n"; continue; }
        std::cout << "    @0.0  = " << _Val(ds->GetValue(0.0)) << "\n";
        std::vector<HdSampledDataSource::Time> ts;
        bool any = ds->GetContributingSampleTimesForInterval(-0.25, 0.25, &ts);
        std::cout << "    contributingSampleTimes(-0.25,0.25) = " << any << " {";
        for (auto t : ts) std::cout << t << " ";
        std::cout << "}\n";
        std::vector<double> raw;
        UsdAttributeQuery(a).GetTimeSamplesInInterval(GfInterval(0, 30), &raw);
        std::cout << "    usd GetTimeSamplesInInterval(0,30) = {";
        for (auto t : raw) std::cout << t << " ";
        std::cout << "}\n";
    }
    std::cout << "  -- stage globals after construction --\n";
    g.Report();

    std::cout << _Hdr("2b. same spline attribute at stage-global time 24");
    {
        ProbeStageGlobals g24(UsdTimeCode(24.0));
        UsdAttribute a = clump.GetAttribute(TfToken("usdGen:clumpRadius"));
        HdSampledDataSourceHandle ds = UsdImagingDataSourceAttributeNew(
            a, g24, sip, HdDataSourceLocator(TfToken("usdGen"),
                                             TfToken("clumpRadius")));
        std::cout << "  @0.0 (t=24) = " << _Val(ds->GetValue(0.0)) << "\n";
        UsdAttribute f = clump.GetAttribute(TfToken("usdGen:frizzAmount"));
        HdSampledDataSourceHandle fds = UsdImagingDataSourceAttributeNew(
            f, g24, sip, HdDataSourceLocator(TfToken("usdGen"),
                                             TfToken("frizzAmount")));
        std::cout << "  frizz @0.0 (t=24) = " << _Val(fds->GetValue(0.0)) << "\n";
    }

    // --------------------------------------------------------------- part 3
    std::cout << _Hdr("3. relationships");
    {
        UsdRelationship rel = clump.GetRelationship(TfToken("usdGen:inputs"));
        auto rds = UsdImagingDataSourceRelationship::New(rel, g);
        std::cout << "  usdGen:inputs -> " << _Val(rds->GetValue(0.0)) << "\n";
        std::vector<HdSampledDataSource::Time> ts;
        std::cout << "  contributingSampleTimes = "
                  << rds->GetContributingSampleTimesForInterval(-1, 1, &ts)
                  << "\n";
        UsdRelationship rel2 = clump.GetRelationship(TfToken("usdGen:surface"));
        auto rds2 = UsdImagingDataSourceRelationship::New(rel2, g);
        std::cout << "  usdGen:surface -> " << _Val(rds2->GetValue(0.0)) << "\n";
    }

    // --------------------------------------------------------------- part 4
    std::cout << _Hdr("4. UsdImagingDataSourceMapped with nested locators + "
                      "custom REST factory");
    {
        using M = UsdImagingDataSourceMapped;
        std::vector<M::PropertyMapping> mappings = {
            M::AttributeMapping{
                {TfToken("usdGen:map"), HdDataSourceLocator(TfToken("map"))}},
            M::AttributeMapping{
                {TfToken("usdGen:ramp:knots"),
                 HdDataSourceLocator(TfToken("ramp"), TfToken("knots"))}},
            M::AttributeMapping{
                {TfToken("usdGen:ramp:values"),
                 HdDataSourceLocator(TfToken("ramp"), TfToken("values"))}},
            M::AttributeMapping{
                {TfToken("usdGen:clumpRadius"),
                 HdDataSourceLocator(TfToken("clumpRadius"))}},
            M::RelationshipMapping{
                {TfToken("usdGen:surface"), HdDataSourceLocator(TfToken("surface"))},
                M::GetPathArrayFromRelationshipDataSourceFactory()},
            M::RelationshipMapping{
                {TfToken("usdGen:inputs"), HdDataSourceLocator(TfToken("inputs"))},
                M::GetPathFromRelationshipDataSourceFactory()},
        };
        static const M::PropertyMappings pm(
            mappings, HdDataSourceLocator(TfToken("usdGen")));
        ProbeStageGlobals gm(UsdTimeCode(24.0));
        auto ds = M::New(clump, sip, pm, gm);
        std::cout << "  container names:";
        for (const TfToken &n : ds->GetNames()) std::cout << " " << n;
        std::cout << "\n";
        _Dump(ds, "    ", 4);
        std::cout << "  Invalidate({usdGen:ramp:knots, usdGen:surface}) -> ";
        HdDataSourceLocatorSet inval = M::Invalidate(
            {TfToken("usdGen:ramp:knots"), TfToken("usdGen:surface")}, pm);
        for (const HdDataSourceLocator &l : inval)
            std::cout << "'" << l.GetString() << "' ";
        std::cout << "\n";
        std::cout << "  -- mapped stage globals --\n";
        gm.Report();
    }

    std::cout << _Hdr("5. REST transport: custom factory sampling "
                      "UsdTimeCode::Default()");
    {
        using M = UsdImagingDataSourceMapped;
        UsdPrim scalp = stage->GetPrimAtPath(SdfPath("/World/Scalp"));
        std::vector<M::PropertyMapping> mappings = {
            M::AttributeMapping{
                {TfToken("points"), HdDataSourceLocator(TfToken("deformed"))}},
            M::AttributeMapping{
                {TfToken("points"), HdDataSourceLocator(TfToken("rest"))},
                M::DataSourceAttributeFactoryFn(_RestPointsFactory)},
        };
        static const M::PropertyMappings pm(
            mappings, HdDataSourceLocator(TfToken("usdGenRest")));
        ProbeStageGlobals g24(UsdTimeCode(24.0));
        auto ds = M::New(scalp, SdfPath("/World/Scalp"), pm, g24);
        _Dump(ds, "    ", 3);
        std::cout << "  -- rest-factory stage globals (should show only "
                     "'usdGenRest/deformed' as time varying) --\n";
        g24.Report();
    }

    // --------------------------------------------------------------- part 6
    std::cout << _Hdr("6. UsdImagingStageSceneIndex has no GetStage(); "
                      "can a downstream index recover a stage?");
    {
        std::cout << "  terminal scene index typeid: "
                  << TfType::Find(*terminal).GetTypeName() << "\n";
        std::cout << "  displayName: " << terminal->GetDisplayName() << "\n";
    }
    std::cout << "\nDONE\n";
    return 0;
}
