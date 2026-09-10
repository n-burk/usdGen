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
#include "usdGenImaging/usdGenImagingSession.h"
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
    // 06 §3.7 stage-key contract: the groom prim adapter stashes the live
    // stage in the weak-stage registry keyed by groom root while the stage
    // index builds data sources — the only place a stage index with no C
    // ABI stage handle can learn the stage. Deterministic: runs at data
    // source build, strictly before adoption/first commit.
    if (prim.GetPrimTypeInfo().GetTypeName() == TfToken("UsdGenGroom")) {
        ::usdGenImaging::UsdGenSessionStore::SetStage(
            prim.GetPath(), prim.GetStage());
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
            // Sibling set for the ancestor pass (contract S3.1): the mapping
            // is built per schema type name, so the full property set is in
            // hand — no new USD calls.
            TfTokenVector const &siblings = def->GetPropertyNames();
            for (TfToken const &name : siblings) {
                if (_IsPrimBuiltin(name)) {
                    continue;
                }
                switch (def->GetSpecType(name)) {
                case SdfSpecTypeAttribute:
                    {
                        UsdImagingDataSourceMapped::AttributeMapping m;
                        m.usdName = name;
                        m.hdLocator = LocatorForProperty(
                            name, /*isRelationship=*/false, siblings);
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
                    m.hdLocator = LocatorForProperty(
                        name, /*isRelationship=*/true, siblings);
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

// Split per 02 §0.7 (relative elements, no container prefix).
static std::vector<std::string>
_Split02(TfToken const &property)
{
    const std::string s = property.GetString();
    const size_t prefix = std::string("usdGen:").size();
    const std::string rest =
        (s.size() >= prefix && s.compare(0, prefix, "usdGen:") == 0)
            ? s.substr(prefix)
            : s;
    std::vector<std::string> elements;
    size_t begin = 0;
    while (begin <= rest.size()) {
        const size_t pos = rest.find(':', begin);
        elements.push_back(
            pos == std::string::npos
                ? rest.substr(begin)
                : rest.substr(begin, pos - begin));
        if (pos == std::string::npos) {
            break;
        }
        begin = pos + 1;
    }
    if (elements.empty()) {
        elements.push_back(s);
    }
    return elements;
}

HdDataSourceLocator
UsdGenPrimAdapterBase::LocatorForProperty(TfToken const &property,
                                         bool isRelationship,
                                         TfTokenVector const &siblings)
{
    // 06 §2.2 rule 4 as amended by the locator contract (.omp/locator-
    // contract.md §2 FINAL RULE): base rule unchanged (02 §0.7 — strip
    // "usdGen:", split rest on ':' → clump/size, frozen/mode); Mappings()
    // prepends the `usdGen` container prefix. DEVIATION & REASON: when this
    // property's base locator is a STRICT ancestor of any sibling's base
    // locator on the same prim type, the ancestor's FINAL element takes
    // "-value" (attribute) / "-rel" (relationship). Without it, an
    // ancestor/descendant pair (usdGen:length vs usdGen:length:source)
    // registers a leaf over a container node and UsdImagingDataSourceMapped
    // TF_CODING_ERRORs (dataSourceMapped.cpp:272 "already an ascendant
    // locator") and drops the container. Descendants NEVER move (R25
    // longest-prefix matching preserved); non-colliding properties keep
    // 02 §0.7 locators exactly (all 02 §6.1 rel rows unchanged).
    // Pure: same (property, isRelationship, siblings) -> same locator.
    std::vector<std::string> elements = _Split02(property);
    for (TfToken const &sib : siblings) {
        if (sib == property) continue;
        std::vector<std::string> sels = _Split02(sib);
        if (elements.size() < sels.size() &&
            std::equal(elements.begin(), elements.end(), sels.begin())) {
            elements.back() += isRelationship ? "-rel" : "-value";
            break;  // one suffix suffices: suffixing only ever extends the
                    // ancestor side, so no two suffixed forms collide.
        }
    }
    TfTokenVector tokens;
    tokens.reserve(elements.size());
    for (std::string const &e : elements) tokens.push_back(TfToken(e));
    return HdDataSourceLocator(tokens.size(), tokens.data());
}

HdDataSourceLocator
UsdGenPrimAdapterBase::LocatorForProperty(TfToken const &property)
{
    // Single-property form: no sibling set, so no ancestor check. Used by
    // test/diagnostic paths that resolve one name outside a type context;
    // Mappings() always uses the 3-arg form above.
    std::vector<std::string> elements = _Split02(property);
    TfTokenVector tokens;
    tokens.reserve(elements.size());
    for (std::string const &e : elements) tokens.push_back(TfToken(e));
    return HdDataSourceLocator(tokens.size(), tokens.data());
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
