// usdGen imaging — UsdGenDirtyRouter implementation (06-imaging.md §3.3,
// 03 §5.1/§5.2 hop 1). Table construction from a compiled graph; routing is
// one hash lookup per notice entry plus a longest-prefix walk (I7, S17).
#include "usdGenImaging/usdGenDirtyRouter.h"

#include "usdGenImaging/primAdapter.h"
#include "usdGenImaging/usdGenTokens.h"

#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE

namespace usdGenImaging {

namespace {

HdDataSourceLocator
_Loc(std::initializer_list<TfToken> tokens)
{
    HdDataSourceLocator loc;
    for (TfToken const &t : tokens) {
        loc = loc.Append(t);
    }
    return loc;
}

/// Locator for a routed parameter. Node paramRouting stores the STRIPPED
/// name (usdGen: removed by the builder, 02 §0.7), so re-add the prefix
/// before handing it to the shared property->locator mapping.
HdDataSourceLocator
_ParamLocator(TfToken const &strippedName)
{
    std::string s = strippedName.GetString();
    if (s.compare(0, 7, "usdGen:") != 0) {
        s = "usdGen:" + s;
    }
    return HdDataSourceLocator(UsdGenContainerToken())
        .Append(UsdGenPrimAdapterBase::LocatorForProperty(TfToken(s)));
}

size_t
_LocatorDepth(HdDataSourceLocator const &loc)
{
    return loc.GetElementCount();
}

// One TF_WARN per (prim, container-leaf) that routes nowhere (06 §9:
// "reported once per groom", not per frame).
void
_WarnUnknownRoute(SdfPath const &path, TfToken const &leaf)
{
    static std::mutex mutex;
    static std::unordered_map<SdfPath, std::set<TfToken>,
                              SdfPathHash> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned[path].insert(leaf).second) {
        TF_WARN("usdGen: dirty locator '%s' on %s matches no routed property "
                "(compiled graph may be stale, or the property is not part "
                "of any operator).",
                TfToken(leaf.GetString()).GetString().c_str(),
                path.GetText());
    }
}

}  // namespace

void
UsdGenDirtyRouter::Rebuild(usdGen::UsdGenGraph const &graph)
{
    _byPrim.clear();
    _entryCount = 0;
    if (graph.NodeCount() == 0) {
        return;  // nothing compiled: route nothing, warn on demand
    }
    usdGen::UsdGenGraphDesc const &desc = graph.Desc();

    std::map<SdfPath, PerPrim> table;

    for (int i = 0, n = graph.NodeCount(); i < n; ++i) {
        usdGen::UsdGenCompiledNode const &node = graph.Node(i);
        if (!node.desc) {
            continue;
        }

        // 02 §6 value rows: every routed parameter of the node.
        for (auto const &routing : node.paramRouting) {
            table[node.desc->path].prefixes.emplace_back(
                _ParamLocator(routing.first),
                Entry{node.id, routing.second});
        }

        // 02 §6 surface rows: deformation of the node's bound surface and
        // the C3 source behind each curve / map ref (06 §3.6: authored curve
        // prims dirty via their own prims, routed as capture/map re-capture).
        if (node.hasSurface && node.surface < desc.surfaces.size()) {
            PerPrim &pp = table[desc.surfaces[node.surface].path];
            Entry const e{node.id, usdGen::UsdGenDirtySurfacePoints,
                          node.surface, true};
            pp.prefixes.emplace_back(
                _Loc({TfToken("primvars"), TfToken("points")}), e);
            pp.prefixes.emplace_back(
                _Loc({TfToken("primvars"), TfToken("points"),
                      TfToken("primvarValue")}),
                e);
            pp.prefixes.emplace_back(
                HdDataSourceLocator(TfToken("extComputationPrimvars")), e);
        }
        // C3 curves changed -> re-capture the consuming node (no curve scope
        // in UsdGenPendingDirty: the capture class carries it, 02 §6 row 5).
        for (SdfPath const &curveRef : node.curveRefs) {
            table[curveRef].prefixes.emplace_back(
                HdDataSourceLocator(),
                Entry{node.id, usdGen::UsdGenDirtyCapture});
        }
        for (SdfPath const &mapRef : node.mapRefs) {
            table[mapRef].prefixes.emplace_back(
                HdDataSourceLocator(UsdGenContainerToken()),
                Entry{node.id, usdGen::UsdGenDirtyMap});
        }
    }

    // usdGen:tileTarget / usdGen:curve:basis on the description (R21): the
    // pending struct has no topology-only scope, so tileTarget routes as
    // Topology on the terminal node — the engine's Repartition consumes it
    // without a digest bump (03 §5.2, types.h UsdGenDirtyTopology comment).
    {
        usdGen::UsdGenNodeId const terminal = graph.TerminalNodeId();
        PerPrim &pp = table[desc.description];
        pp.prefixes.emplace_back(
            _Loc({UsdGenContainerToken(), TfToken("tileTarget")}),
            Entry{terminal, usdGen::UsdGenDirtyTopology});
        pp.prefixes.emplace_back(
            _Loc({UsdGenContainerToken(), TfToken("curve"), TfToken("basis")}),
            Entry{terminal, usdGen::UsdGenDirtyTopology});
    }

    for (auto &primEntry : table) {
        // Longest prefix first: a dirty on usdGen/clump matches every
        // usdGen/clump/* leaf; the Universal/marker prefixes sink to the end.
        std::sort(primEntry.second.prefixes.begin(),
                  primEntry.second.prefixes.end(),
                  [](std::pair<HdDataSourceLocator, Entry> const &a,
                     std::pair<HdDataSourceLocator, Entry> const &b) {
                      return _LocatorDepth(a.first) > _LocatorDepth(b.first);
                  });
        _entryCount += primEntry.second.prefixes.size();
        _byPrim.emplace(primEntry.first, std::move(primEntry.second));
    }
}

void
UsdGenDirtyRouter::Route(
    HdSceneIndexObserver::DirtiedPrimEntries const &entries,
    usdGen::UsdGenPendingDirty *out) const
{
    static HdDataSourceLocator const s_container(UsdGenContainerToken());

    for (auto const &entry : entries) {
        auto const it = _byPrim.find(entry.primPath);
        if (it == _byPrim.end()) {
            // Not a routed graph prim. (Scene-frame dirty on '/' is detected
            // by the scene index itself — 06 §3.9 trigger (b).)
            continue;
        }
        PerPrim const &pp = it->second;

        bool const universal = entry.dirtyLocators.IsEmpty() ||
            entry.dirtyLocators == HdDataSourceLocatorSet::UniversalSet();
        if (universal) {
            // Full-prim resync (02 §6.6 rule 2): surfaces resync as surface
            // topology; node prims resync structurally.
            for (auto const &prefix : pp.prefixes) {
                if (prefix.second.surfaceScoped) {
                    out->surfaceBits[prefix.second.surface] |=
                        usdGen::UsdGenDirtySurfaceTopo;
                    out->surfaceTopology = true;
                } else {
                    out->structural = true;
                }
            }
            continue;
        }

        for (auto const &locator : entry.dirtyLocators) {
            bool matched = false;
            for (auto const &prefix : pp.prefixes) {
                // Intersects: locator is under the table prefix, equal to
                // it, or COARSER than it (a container resync dirties every
                // routed leaf beneath). Universal entries store the empty
                // locator, which is a prefix of everything.
                if (locator.Intersects(prefix.first)) {
                    if (prefix.second.surfaceScoped) {
                        out->surfaceBits[prefix.second.surface] |=
                            prefix.second.bits;
                    } else {
                        out->nodeBits[prefix.second.node] |= prefix.second.bits;
                    }
                    matched = true;
                    break;  // longest prefix wins (sorted in Rebuild)
                }
            }
            if (matched) {
                continue;
            }
            if (locator == s_container) {
                // Adapter resync of the whole usdGen container (02 §6.6 r2).
                out->structural = true;
            } else if (locator.GetFirstElement() == UsdGenContainerToken()) {
                _WarnUnknownRoute(entry.primPath, locator.GetLastElement());
            }
        }
    }
}


void
UsdGenDirtyRouter::RouteAdded(
    HdSceneIndexObserver::AddedPrimEntries const &entries,
    usdGen::UsdGenPendingDirty *out) const
{
    for (auto const &entry : entries) {
        if (_touchesGraphPath(entry.primPath)) {
            out->structural = true;
        }
    }
}

void
UsdGenDirtyRouter::RouteRemoved(
    HdSceneIndexObserver::RemovedPrimEntries const &entries,
    usdGen::UsdGenPendingDirty *out) const
{
    for (auto const &entry : entries) {
        if (_touchesGraphPath(entry.primPath)) {
            out->structural = true;
        }
    }
}

bool
UsdGenDirtyRouter::_touchesGraphPath(SdfPath const &path) const
{
    for (auto const &primEntry : _byPrim) {
        SdfPath const &key = primEntry.first;
        if (path == key || key.HasPrefix(path) || path.HasPrefix(key)) {
            return true;
        }
    }
    return false;
}

}  // namespace usdGenImaging

PXR_NAMESPACE_CLOSE_SCOPE
