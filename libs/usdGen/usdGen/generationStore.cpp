// usdGen engine — generation store implementation.
//
// Publish/Get use std::atomic_load/std::atomic_store on the shared_ptr (I7,
// 03 §6.1): NOT TfRefPtr, which has no atomic overloads in 26.08. This is
// exactly the pattern usdRig ships (snapshotStore.h:330, 369-377) and the one
// validated by gate SI-4 (8 readers x 100 publishes, 0 torn reads).
#include "usdGen/generationStore.h"

#include "pxr/pxr.h"
#include "pxr/base/vt/array.h"
#include <algorithm>

#include <atomic>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

UsdGenGenerationStore::UsdGenGenerationStore(UsdGenGenerationConstPtr baseline)
    : _current(std::move(baseline))
{
    if (_current) _nextId = _current->id + 1;
}

void UsdGenGenerationStore::Publish(UsdGenGeneration gen)
{
    // Zero-based ids: the first published generation has id 0, so across any
    // published history `id == Stats().commits - 1` (03 §9: exactly one publish
    // per committed request; superseded or aborted commits allocate no id).
    gen.id = _nextId++;
    UsdGenGenerationConstPtr genConst =
        std::make_shared<const UsdGenGeneration>(std::move(gen));
    std::atomic_store(&_current, genConst);
}

UsdGenGenerationConstPtr UsdGenGenerationStore::Get() const noexcept
{
    return std::atomic_load(&_current);
}

int64_t UsdGenGenerationStore::NextId() const noexcept { return _nextId; }

bool UsdGenPrimSetSignature::operator==(
    UsdGenPrimSetSignature const &rhs) const
{
    return tileCount == rhs.tileCount &&
           instancerCount == rhs.instancerCount &&
           primPaths == rhs.primPaths &&
           primvarNames == rhs.primvarNames &&
           primTypes == rhs.primTypes;
}

namespace {

// Per-prim primvar-name list from a signature; empty when the path is new.
std::vector<std::string> const &
_namesFor(UsdGenPrimSetSignature const &sig, std::string const &path)
{
    for (size_t i = 0; i < sig.primPaths.size(); ++i)
        if (sig.primPaths[i] == path)
            return sig.primvarNames[i];
    static const std::vector<std::string> kEmpty;
    return kEmpty;
}

// next-names minus prev-names, both kept sorted by the builder.
std::vector<TfToken>
_newPrimvars(UsdGenPrimSetSignature const &prev, std::string const &prevPath,
             UsdGenPrimSetSignature const &next, std::string const &nextPath)
{
    std::vector<std::string> const &a = _namesFor(prev, prevPath);
    std::vector<std::string> const &b = _namesFor(next, nextPath);
    std::vector<TfToken> out;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j])                ++i;
        else if (b[j] < a[i])           out.emplace_back(b[j++]);
        else                            { ++i; ++j; }
    }
    while (j < b.size()) out.emplace_back(b[j++]);
    return out;
}

template <typename T>
bool _changed(VtArray<T> const &a, VtArray<T> const &b)
{
    return !a.IsIdentical(b);   // data-pointer compare (03 §6.1), never memcmp
}

}  // namespace

UsdGenDirtyReport UsdGenGenerationStore::Diff(
    UsdGenGeneration const &prev, UsdGenGeneration const &next) const
{
    // Both tile vectors are sorted by tile id (UsdGenGeneration invariant),
    // so a single merge walk classifies every prim.
    UsdGenDirtyReport report;
    size_t i = 0, j = 0;
    while (i < prev.tiles.size() || j < next.tiles.size()) {
        UsdGenTileDirty td;
        if (j < next.tiles.size() &&
            (i >= prev.tiles.size() || next.tiles[j].tile < prev.tiles[i].tile))
        {   // prim appears only in next -> added.
            td.tile = next.tiles[j].tile;
            td.primPath = next.tiles[j].primPath;
            td.added = true;
            td.pointsDirty = true;   // fresh prim: everything is new content
            td.newPrimvars = _newPrimvars(prev.signature, td.primPath.GetAsString(),
                                          next.signature, td.primPath.GetAsString());
            ++j;
        }
        else if (i < prev.tiles.size() &&
                 (j >= next.tiles.size() || prev.tiles[i].tile < next.tiles[j].tile))
        {   // prim disappears -> removed.
            td.tile = prev.tiles[i].tile;
            td.primPath = prev.tiles[i].primPath;
            td.removed = true;
            ++i;
        }
        else {
            UsdGenTilePublication const &a = prev.tiles[i];
            UsdGenTilePublication const &b = next.tiles[j];
            td.tile = b.tile;
            td.primPath = b.primPath;
            td.pointsDirty = _changed(a.points, b.points);
            td.widthsDirty = _changed(a.widths, b.widths);
            td.xformDirty  = !(a.xformMatrix == b.xformMatrix);
            auto mark = [&td](TfToken const &name, bool dirty) {
                if (dirty) td.dirtyPrimvars.push_back(name);
            };
            mark(TfToken("hairT"),  _changed(a.hairT,  b.hairT));
            mark(TfToken("hairId"), _changed(a.hairId, b.hairId));
            mark(TfToken("st"),     _changed(a.st,     b.st));
            mark(TfToken("displayColor"), _changed(a.displayColor, b.displayColor));
            mark(TfToken("bakeColor"),    _changed(a.bakeColor,    b.bakeColor));
            mark(TfToken("velocities"),   _changed(a.velocities,   b.velocities));
            // Named uniform planes (both vectors sorted by name): common names
            // compared by content, next-only names are new primvars.
            for (UsdGenPlane const &pb : b.extraUniform) {
                bool paired = false;
                for (UsdGenPlane const &pa : a.extraUniform) {
                    if (pa.name != pb.name) continue;
                    paired = true;
                    mark(pb.name, pa.f != pb.f || pa.i != pb.i);
                    break;
                }
                if (!paired) td.newPrimvars.push_back(pb.name);
            }
            // Primvar-set growth not visible in payloads (e.g. empty->empty)
            // still comes from the signatures.
            std::vector<TfToken> sigNew =
                _newPrimvars(prev.signature, std::string(a.primPath.GetAsString()),
                             next.signature, std::string(b.primPath.GetAsString()));
            for (TfToken const &t : sigNew) {
                if (std::find(td.newPrimvars.begin(), td.newPrimvars.end(), t)
                    == td.newPrimvars.end())
                    td.newPrimvars.push_back(t);
            }
            ++i;
            ++j;
        }
        report.tiles.push_back(std::move(td));
    }
    // Surface xform dirt is a scene-index concern the engine cannot observe;
    // the imaging dirty router sets this bit itself.
    report.surfaceXformDirty = false;
    return report;
}

}  // namespace usdGen
