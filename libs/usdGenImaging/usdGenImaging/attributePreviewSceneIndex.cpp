// usdGen imaging — the live paint-preview overlay implementation.
#include "usdGenImaging/attributePreviewSceneIndex.h"

#include "usdGen/valuePreview.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/meshTopologySchema.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

// The render-chain registration: every renderer gets one preview index after
// the groom index (plugInfo ordering), attached to the global registry the
// brush C ABI writes.
TF_DEFINE_ENV_SETTING(USDGEN_BRUSH_PREVIEW_ENABLE, true,
                      "Enable the usdGen attribute-brush preview overlay scene index.");

class UsdGenAttributePreviewSceneIndexPlugin : public HdSceneIndexPlugin {
public:
    UsdGenAttributePreviewSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(std::string const &,
                                             HdSceneIndexBaseRefPtr const &input,
                                             HdContainerDataSourceHandle const &) override
    {
        return usdGenImaging::UsdGenAttributePreviewSceneIndex::New(input, true);
    }
    bool _IsEnabled(HdContainerDataSourceHandle const &) const override
    {
        return TfGetEnvSetting(USDGEN_BRUSH_PREVIEW_ENABLE);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<UsdGenAttributePreviewSceneIndexPlugin>();
}
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("UsdGenAttributePreviewSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

PXR_NAMESPACE_CLOSE_SCOPE

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

namespace {

// Above this face resolution the per-face mean is a GRID x GRID bilinear
// sample grid rather than every texel (see the header).
constexpr int kExactRes = 4;
constexpr int kGrid = 4;

GfVec3f FaceColor(usdGen::UsdGenAttributeMap const &map, int face, int channel,
                  TfToken const &colorMap, GfVec2f const &range)
{
    int const res = map.Resolution();
    double sum = 0.0;
    size_t n = 0;
    if (res <= kExactRes) {
        for (int t = 0; t < res; ++t) {
            for (int s = 0; s < res; ++s) {
                float v = 0.0f;
                if (map.GetTexel(face, s, t, channel, &v)) {
                    sum += v;
                    ++n;
                }
            }
        }
    } else {
        for (int t = 0; t < kGrid; ++t) {
            for (int s = 0; s < kGrid; ++s) {
                float v = 0.0f;
                if (map.Sample(face, (float(s) + 0.5f) / kGrid,
                               (float(t) + 0.5f) / kGrid, channel,
                               usdGen::UsdGenAttributeMapInterp::Bilinear, &v)) {
                    sum += v;
                    ++n;
                }
            }
        }
    }
    if (n == 0) return usdGen::UsdGenPreviewMissingColor();
    double const mean = sum / double(n);
    return usdGen::UsdGenPreviewColor(colorMap, range, &mean, 1);
}

HdContainerDataSourceHandle DisplayColorOverlay(VtVec3fArray const &colors,
                                                bool faceVarying)
{
    static TfToken const uniform("uniform");
    static TfToken const fv("faceVarying");
    // HdOverlayContainerDataSource merges nested containers, so an upstream
    // INDEXED displayColor would keep its indexedPrimvarValue + indices, and
    // HdPrimvarSchema::IsIndexed (both present) makes the scene-delegate
    // adapter read those instead of our primvarValue. Block both.
    TfToken const names[] = {
        HdPrimvarSchemaTokens->primvarValue,
        HdPrimvarSchemaTokens->interpolation,
        HdPrimvarSchemaTokens->role,
        HdPrimvarSchemaTokens->indexedPrimvarValue,
        HdPrimvarSchemaTokens->indices,
    };
    HdDataSourceBaseHandle const values[] = {
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(colors),
        HdRetainedTypedSampledDataSource<TfToken>::New(faceVarying ? fv : uniform),
        HdRetainedTypedSampledDataSource<TfToken>::New(TfToken("color")),
        HdBlockDataSource::New(),
        HdBlockDataSource::New(),
    };
    return HdRetainedContainerDataSource::New(
        TfToken("primvars"),
        HdRetainedContainerDataSource::New(
            TfToken("displayColor"),
            HdRetainedContainerDataSource::New(5, names, values)));
}

// Face-vertex count of an upstream mesh prim, or -1 when it is not a mesh
// (or states no topology).
int64_t FaceVertexCount(HdContainerDataSourceHandle const &prim)
{
    HdMeshSchema const mesh = HdMeshSchema::GetFromParent(prim);
    if (!mesh.IsDefined()) return -1;
    HdMeshTopologySchema const topology = mesh.GetTopology();
    if (!topology.IsDefined()) return -1;
    HdIntArrayDataSourceHandle const indices = topology.GetFaceVertexIndices();
    if (!indices) return -1;
    return int64_t(indices->GetTypedValue(0.0f).size());
}

HdDataSourceLocator const &StructuralLocator()
{
    static HdDataSourceLocator const l(TfToken("primvars"), TfToken("displayColor"));
    return l;
}
HdDataSourceLocator const &ValueLocator()
{
    static HdDataSourceLocator const l(TfToken("primvars"), TfToken("displayColor"),
                                       TfToken("primvarValue"));
    return l;
}

}  // namespace

// ---------------------------------------------------------------------------
// UsdGenAttributePreviewRegistry
// ---------------------------------------------------------------------------

UsdGenAttributePreviewRegistry &UsdGenAttributePreviewRegistry::Get()
{
    static UsdGenAttributePreviewRegistry *registry = new UsdGenAttributePreviewRegistry();
    return *registry;
}

bool UsdGenAttributePreviewRegistry::Set(SdfPath const &target, VtVec3fArray const &colors)
{
    if (target.IsEmpty() || colors.empty()) return false;
    bool structural = false;
    {
        std::lock_guard<std::mutex> lock(_entriesMutex);
        auto found = _entries.find(target);
        if (found == _entries.end()) {
            structural = true;
            _entries.emplace(target, colors);
        } else {
            // A size change is an interpolation-level change for Hydra's
            // primvar descriptors: republish structurally.
            structural = found->second.size() != colors.size();
            found->second = colors;
        }
    }
    _Notify(target, structural);
    return true;
}

bool UsdGenAttributePreviewRegistry::Clear(SdfPath const &target)
{
    {
        std::lock_guard<std::mutex> lock(_entriesMutex);
        if (_entries.erase(target) == 0) return false;
    }
    _Notify(target, true);
    return true;
}

size_t UsdGenAttributePreviewRegistry::ClearAll()
{
    std::vector<SdfPath> cleared;
    {
        std::lock_guard<std::mutex> lock(_entriesMutex);
        cleared.reserve(_entries.size());
        for (auto const &entry : _entries) cleared.push_back(entry.first);
        _entries.clear();
    }
    for (SdfPath const &path : cleared) _Notify(path, true);
    return cleared.size();
}

bool UsdGenAttributePreviewRegistry::Lookup(SdfPath const &target, VtVec3fArray *colors) const
{
    std::lock_guard<std::mutex> lock(_entriesMutex);
    auto found = _entries.find(target);
    if (found == _entries.end()) return false;
    if (colors) *colors = found->second;
    return true;
}

size_t UsdGenAttributePreviewRegistry::EntryCount() const
{
    std::lock_guard<std::mutex> lock(_entriesMutex);
    return _entries.size();
}

void UsdGenAttributePreviewRegistry::Attach(UsdGenAttributePreviewSceneIndex *index)
{
    std::lock_guard<std::recursive_mutex> lock(_indicesMutex);
    if (std::find(_indices.begin(), _indices.end(), index) == _indices.end())
        _indices.push_back(index);
}

void UsdGenAttributePreviewRegistry::Detach(UsdGenAttributePreviewSceneIndex *index)
{
    std::lock_guard<std::recursive_mutex> lock(_indicesMutex);
    _indices.erase(std::remove(_indices.begin(), _indices.end(), index), _indices.end());
}

size_t UsdGenAttributePreviewRegistry::IndexCount() const
{
    std::lock_guard<std::recursive_mutex> lock(_indicesMutex);
    return _indices.size();
}

void UsdGenAttributePreviewRegistry::_Notify(SdfPath const &target, bool structural)
{
    std::lock_guard<std::recursive_mutex> lock(_indicesMutex);
    // Copy: an observer could construct or destroy an index while notified.
    std::vector<UsdGenAttributePreviewSceneIndex *> const indices = _indices;
    for (UsdGenAttributePreviewSceneIndex *index : indices) {
        if (std::find(_indices.begin(), _indices.end(), index) == _indices.end()) continue;
        index->_NotifyRegistryChange(target, structural);
    }
}

// ---------------------------------------------------------------------------
// UsdGenAttributePreviewSceneIndex
// ---------------------------------------------------------------------------

UsdGenAttributePreviewSceneIndex::UsdGenAttributePreviewSceneIndex(
    HdSceneIndexBaseRefPtr const &input, bool attachGlobal)
    : HdSingleInputFilteringSceneIndexBase(input), _attached(attachGlobal)
{
    if (_attached) UsdGenAttributePreviewRegistry::Get().Attach(this);
}

UsdGenAttributePreviewSceneIndex::~UsdGenAttributePreviewSceneIndex()
{
    if (_attached) UsdGenAttributePreviewRegistry::Get().Detach(this);
}

void UsdGenAttributePreviewSceneIndex::_NotifyRegistryChange(SdfPath const &target,
                                                             bool structural)
{
    try {
        _SendPrimsDirtied({{target, HdDataSourceLocatorSet{
                                        structural ? StructuralLocator() : ValueLocator()}}});
    } catch (...) {
        TF_WARN("usdGen brush preview: a dirty observer threw");
    }
}

bool UsdGenAttributePreviewSceneIndex::_Store(SdfPath const &target, Entry entry)
{
    bool structural = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto found = _previews.find(target);
        structural = found == _previews.end() ||
                     found->second.faceVarying != entry.faceVarying ||
                     found->second.colors.size() != entry.colors.size();
        _previews[target] = std::move(entry);
    }
    // Structural the first time (the primvar appears), values-only after.
    _SendPrimsDirtied({{target, HdDataSourceLocatorSet{
                                    structural ? StructuralLocator() : ValueLocator()}}});
    return true;
}

bool UsdGenAttributePreviewSceneIndex::SetFaceVaryingPreview(SdfPath const &target,
                                                             VtVec3fArray const &colors)
{
    if (colors.empty()) return false;
    Entry entry;
    entry.colors = colors;
    entry.faceVarying = true;
    return _Store(target, std::move(entry));
}

bool UsdGenAttributePreviewSceneIndex::SetMapPreview(
    SdfPath const &target, std::shared_ptr<const usdGen::UsdGenAttributeMap> map,
    TfToken const &colorMap, GfVec2f const &range, int channel)
{
    if (!map || channel < 0 || channel >= map->Channels()) return false;
    Entry entry;
    entry.colors.resize(size_t(map->NumFaces()));
    for (int f = 0; f < map->NumFaces(); ++f)
        entry.colors[size_t(f)] = FaceColor(*map, f, channel, colorMap, range);
    return _Store(target, std::move(entry));
}

bool UsdGenAttributePreviewSceneIndex::SetGroomPreview(
    SdfPath const &target, std::shared_ptr<const usdGen::UsdGenAttributeMap> map,
    std::vector<int> const &rootFaces, std::vector<GfVec2f> const &rootUV,
    TfToken const &colorMap, GfVec2f const &range, int channel)
{
    if (!map || channel < 0 || channel >= map->Channels()) return false;
    if (rootFaces.empty() || rootFaces.size() != rootUV.size()) return false;
    Entry entry;
    entry.colors.resize(rootFaces.size());
    for (size_t i = 0; i < rootFaces.size(); ++i) {
        float v = 0.0f;
        if (!map->Sample(rootFaces[i], rootUV[i][0], rootUV[i][1], channel,
                         usdGen::UsdGenAttributeMapInterp::Bilinear, &v)) {
            entry.colors[i] = usdGen::UsdGenPreviewMissingColor();
            continue;
        }
        double const d = v;
        entry.colors[i] = usdGen::UsdGenPreviewColor(colorMap, range, &d, 1);
    }
    return _Store(target, std::move(entry));
}

bool UsdGenAttributePreviewSceneIndex::ClearPreview(SdfPath const &target)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_previews.erase(target) == 0) return false;
    }
    _SendPrimsDirtied({{target, HdDataSourceLocatorSet{HdDataSourceLocator(
                                    TfToken("primvars"), TfToken("displayColor"))}}});
    return true;
}

void UsdGenAttributePreviewSceneIndex::ClearAllPreviews()
{
    std::vector<SdfPath> targets;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto const &kv : _previews) targets.push_back(kv.first);
        _previews.clear();
    }
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    for (SdfPath const &target : targets) {
        entries.push_back({target, HdDataSourceLocatorSet{HdDataSourceLocator(
                                        TfToken("primvars"), TfToken("displayColor"))}});
    }
    if (!entries.empty()) _SendPrimsDirtied(entries);
}

bool UsdGenAttributePreviewSceneIndex::HasPreview(SdfPath const &target) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _previews.find(target) != _previews.end();
}

HdSceneIndexPrim UsdGenAttributePreviewSceneIndex::GetPrim(SdfPath const &path) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (!prim.dataSource) return prim;
    VtVec3fArray colors;
    bool faceVarying = false;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const local = _previews.find(path);
        if (local != _previews.end()) {
            colors = local->second.colors;
            faceVarying = local->second.faceVarying;
            found = true;
        }
    }
    if (!found && _attached &&
        UsdGenAttributePreviewRegistry::Get().Lookup(path, &colors)) {
        faceVarying = true;
        found = true;
    }
    if (!found) return prim;
    // A faceVarying overlay must match the mesh it lands on; a stale one
    // (topology edited since the Set) is withheld rather than mis-sized.
    if (faceVarying && FaceVertexCount(prim.dataSource) != int64_t(colors.size()))
        return prim;
    // Upstream first, overlay wins: every other primvar (and the rest of the
    // prim) shows through unchanged. The overlay sets the whole displayColor
    // triple (values, interpolation, role) and blocks the indexed pair
    // (indexedPrimvarValue, indices), so no upstream value field survives.
    prim.dataSource = HdOverlayContainerDataSource::New(
        DisplayColorOverlay(colors, faceVarying), prim.dataSource);
    return prim;
}

SdfPathVector UsdGenAttributePreviewSceneIndex::GetChildPrimPaths(
    SdfPath const &path) const
{
    return _GetInputSceneIndex()->GetChildPrimPaths(path);
}

}  // namespace usdGenImaging
