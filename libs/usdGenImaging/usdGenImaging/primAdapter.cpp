// usdGen prim adapters (ADR §5.1 #3; 06-imaging.md §2).
//
// One adapter body under five plugInfo entries (06 §2.1): GetImagingSubprims
// returns {TfToken()} — one Hydra prim per USD prim — and
// GetImagingSubprimData overlays a single `usdGen` container (built by
// UsdImagingDataSourceMapped) onto UsdImagingDataSourcePrim.
//
// The mapping table is built GENERICALLY from UsdPrimDefinition (06 §2.2
// rules 1-4; R6: a skip-list, never a name-prefix test):
//   * every SdfSpecTypeAttribute  -> nested usdGen/<a>/<b> leaf
//   * every SdfSpecTypeRelationship -> usdGen/<name> path (array) or
//     single-path source, per the 02 §2.3 "exactly one target" set
//   * builtins already served by UsdImagingDataSourcePrim (visibility,
//     purpose, proxyPrim, xformOpOrder, xformOp:*, extent, primvars:*) are
//     the ONLY skipped names.
// A `float …:spline` ramp property (06 §2.4) is transported as the whole
// TsSpline via a custom factory and is never flagged time-varying.
//
// Invalidation (06 §2.7): 1:1 nested-locator invalidation from
// UsdImagingDataSourceMapped::Invalidate unioned with
// UsdImagingDataSourcePrim::Invalidate so the three Imageable types keep
// visibility/purpose/extent/primvars working. A resync of the bare `usdGen`
// container is emitted by the mapped source when a property appears or
// disappears; the dirty router treats that as graph-structural (02 §6.6
// rule 2).
#include "usdGenImaging/primAdapter.h"

#include "pxr/base/ts/spline.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usdImaging/usdImaging/dataSourceMapped.h"
#include "pxr/usdImaging/usdImaging/dataSourcePrim.h"
#include "pxr/base/tf/hash.h"

#include <mutex>
#include <string>
#include <unordered_map>

#include "usdGenImaging/usdGenTokens.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// 06 §2.2 rule 3: the skip-list is exactly what
bool
_IsPrimBuiltin(TfToken const &name)
{
    // 06 §2.2 rule 3: skip ONLY what UsdImagingDataSourcePrim already serves.
    if (name == TfToken("visibility") || name == TfToken("purpose") ||
        name == TfToken("proxyPrim") || name == TfToken("xformOpOrder") ||
        name == TfToken("extent")) {
        return true;
    }
    const std::string s = name.GetString();
    return s.rfind("xformOp:", 0) == 0 || s.rfind("primvars:", 0) == 0;
}

// 06 §2.4 / S11: whole-spline ramp transport. Reads the authored spline and
// returns a retained TsSpline source WITHOUT flagging time-varying (a .spline
// on a plain float would otherwise dirty on every SetTime — MEASURED probe3).
HdSampledDataSourceHandle
_SplineRampFactory(
    UsdAttribute const &attr,
    UsdImagingDataSourceStageGlobals const &,
    SdfPath const &,
    HdDataSourceLocator const &)
{
    TsSpline spline = attr.GetSpline();
    if (spline.IsEmpty()) {
        return nullptr;  // no spline authored: no opinion at this leaf
    }
    return HdRetainedTypedSampledDataSource<TsSpline>::New(spline);
}

}  // namespace

TfTokenVector
UsdGenPrimAdapterBase::GetImagingSubprims(UsdPrim const &)
{
    // 06 §2.1: one Hydra prim per USD prim, no subprims.
    return TfTokenVector{ TfToken() };
}

TfToken
UsdGenPrimAdapterBase::GetImagingSubprimType(
        UsdPrim const &, TfToken const &)
{
    return TfToken();
}

HdContainerDataSourceHandle
UsdGenPrimAdapterBase::GetImagingSubprimData(
        UsdPrim const &prim,
        TfToken const &subprim,
        const UsdImagingDataSourceStageGlobals &stageGlobals)
{
    if (subprim != TfToken()) {
        return nullptr;
    }
    HdContainerDataSourceHandle base =
        UsdImagingDataSourcePrim::New(
            prim.GetPath(), prim, stageGlobals);
    HdContainerDataSourceHandle usdGen =
        UsdImagingDataSourceMapped::New(
            prim, prim.GetPath(),
            Mappings(prim.GetPrimTypeInfo().GetSchemaTypeName()),
            stageGlobals);
    if (!usdGen) {
        return base;
    }
    // The usdGen container and the prim-level containers are disjoint
    // locators; the overlay exists so downstream flattening sees one root.
    return HdOverlayContainerDataSource::OverlayedContainerDataSources(
        usdGen, base);
}

HdDataSourceLocatorSet
UsdGenPrimAdapterBase::InvalidateImagingSubprim(
        UsdPrim const &prim,
        TfToken const &subprim,
        TfTokenVector const &properties,
        UsdImagingPropertyInvalidationType invalidationType)
{
    // 06 §2.7: the mapped source gives 1:1 nested-locator invalidation for
    // every usdGen:* property edit (usdGen:noise:magnitude ->
    // usdGen/noise/magnitude); the union keeps visibility/purpose/extent/
    // primvars working on the Imageable types. Taken unconditionally: a
    // dirty for a locator that is never published is ignored by Hydra.
    HdDataSourceLocatorSet result =
        UsdImagingDataSourceMapped::Invalidate(
            properties,
            Mappings(prim.GetPrimTypeInfo().GetSchemaTypeName()));
    result.insert(
        UsdImagingDataSourcePrim::Invalidate(
            prim, subprim, properties, invalidationType));
    return result;
}

const UsdImagingDataSourceMapped::PropertyMappings &
UsdGenPrimAdapterBase::Mappings(TfToken const &schemaTypeName)
{
    static std::unordered_map<
        TfToken,
        std::unique_ptr<UsdImagingDataSourceMapped::PropertyMappings>,
        TfHash> cache;
    static std::mutex mutex;
    // The cache holds pointers into live PropertyMappings; Hydra pulls from
    // several scene-index threads, so lookup and build both run under lock.
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(schemaTypeName); it != cache.end()) {
        return *it->second;
    }
    std::vector<UsdImagingDataSourceMapped::PropertyMapping> mappings;
    // FindConcretePrimDefinition returns nullptr for abstract types and for
    // names with no registered schema — such a prim has no declared
    // usdGen:* properties, so the cached (empty) table is correct (06 §2.2
    // rule 2's null branch).
    if (UsdPrimDefinition const *def =
            UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(schemaTypeName)) {
            for (TfToken const &name : def->GetPropertyNames()) {
                if (_IsPrimBuiltin(name)) {
                    continue;
                }
                switch (def->GetSpecType(name)) {
                case SdfSpecTypeAttribute:
                    {
                        UsdImagingDataSourceMapped::AttributeMapping m;
                        m.usdName = name;
                        m.hdLocator = LocatorForProperty(name);
                        if (const std::string ps = name.GetString();
                            ps.size() >= 7 && ps.compare(ps.size() - 7, 7, ":spline") == 0) {
                            // Whole-spline ramp transport (06 §2.4): the stock
                            // factory would flag a .spline time-varying.
                            m.factory = _SplineRampFactory;
                        }
                        mappings.push_back(std::move(m));
                    }
                    break;
            case SdfSpecTypeRelationship:
                {
                    UsdImagingDataSourceMapped::RelationshipMapping m;
                    m.usdName = name;
                    m.hdLocator = LocatorForProperty(name);
                    m.factory = IsSingleTarget(name)
                        ? UsdImagingDataSourceMapped::GetPathFromRelationshipDataSourceFactory()
                        : UsdImagingDataSourceMapped::GetPathArrayFromRelationshipDataSourceFactory();
                    mappings.push_back(std::move(m));
                }
                break;
            default:
                break;
            }
        }
    }
    auto slot = std::make_unique<UsdImagingDataSourceMapped::PropertyMappings>(
        mappings, HdDataSourceLocator(PXR_NS::usdGenImaging::UsdGenContainerToken()));
    auto res = cache.emplace(schemaTypeName, std::move(slot));
    return *res.first->second;
}

HdDataSourceLocator
UsdGenPrimAdapterBase::LocatorForProperty(TfToken const &property)
{
    // 02-schema.md §0.7: the locator is the property name with the leading
    // usdGen: stripped and the remaining ':' separators turned into '/' —
    // usdGen:clump:size -> clump/size, usdGen:frozen:mode -> frozen/mode.
    // Mappings() prepends the `usdGen` container prefix (06 §2.2 rule 4).
    const std::string s = property.GetString();
    const size_t prefix = std::string("usdGen:").size();
    const std::string rest =
        (s.size() >= prefix && s.compare(0, prefix, "usdGen:") == 0)
            ? s.substr(prefix)
            : s;
    TfTokenVector elements;
    size_t begin = 0;
    while (begin <= rest.size()) {
        const size_t pos = rest.find(':', begin);
        elements.push_back(TfToken(
            pos == std::string::npos
                ? rest.substr(begin)
                : rest.substr(begin, pos - begin)));
        if (pos == std::string::npos) {
            break;
        }
        begin = pos + 1;
    }
    if (elements.empty()) {
        elements.push_back(TfToken(property));
    }
    return HdDataSourceLocator(elements.size(), elements.data());
}

bool
UsdGenPrimAdapterBase::IsSingleTarget(TfToken const &property)
{
    // 06 §2.3: exactly one target. Every other relationship keeps the array
    // factory — an empty array is how the evaluator sees "not bound".
    return property == TfToken("usdGen:terminal") ||
           property == TfToken("usdGen:frozen:curves");
}

#define USDGEN_DEFINE_PRIM_ADAPTER(AdapterType)                              \
    TF_REGISTRY_FUNCTION(TfType) {                                           \
        TfType t = TfType::Define<                                           \
            AdapterType,                                                    \
            TfType::Bases<UsdImagingSceneIndexPrimAdapter>>();               \
        t.SetFactory<UsdImagingPrimAdapterFactory<AdapterType>>();           \
    }

USDGEN_DEFINE_PRIM_ADAPTER(UsdGenGroomAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenDescriptionAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenOperatorAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenMapAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenGuideSetAdapter)

#undef USDGEN_DEFINE_PRIM_ADAPTER

PXR_NAMESPACE_CLOSE_SCOPE
