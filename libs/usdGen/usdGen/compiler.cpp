// usdGen engine — compiler implementation (03-execution-engine.md §3).
//
// Compile() walks UsdGenGraphDesc::nodes and: edges from usdGen:input only
// (S26); Kahn sort with namespace tie-break; cycle detection (compile error
// naming the offending pair); dense node ids in topological order; space /
// readPhase resolution (§1.6); reference-lane ordering (§1.5); OutputPrimvars
// slot binding (§1.2); Merkle structural digests (§3.3); tile arithmetic
// (R21); dirty routing table rebuild data (§5.1).
//
// Recompile() is the incremental path (gate E-6): every node's structural
// digest is recomputed; a node whose digest is unchanged (same type,
// inputs, topology-class params, relationship targets, resolved
// space/readPhase) keeps its op, capture and buffer from the previous
// graph. Exactly one appended node therefore rebuilds exactly one node.
#include "usdGen/compiler.h"

#include "usdGen/opRegistry.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE


namespace usdGen {
namespace {

// FNV-1a 64-bit.
uint64_t Fnv1a(uint64_t h, void const *data, size_t n)
{
    auto const *p = static_cast<unsigned char const *>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

uint64_t Fnv1aI64(uint64_t h, int64_t v)
{
    uint64_t u = static_cast<uint64_t>(v);
    return Fnv1a(h, &u, sizeof(u));
}
uint64_t Fnv1aCstr(uint64_t h, char const *s)
{
    for (auto const *p = reinterpret_cast<unsigned char const *>(s); *p; ++p) {
        h ^= *p; h *= 0x100000001b3ULL;
    }
    return h;
}
uint64_t Fnv1aTfToken(uint64_t h, TfToken const &t)
{
    // GetText(): the interned c-string; GetString() would allocate a
    // std::string per call on the E-6 hot path.
    return Fnv1aI64(Fnv1aCstr(h, t.GetText()),
                    static_cast<int64_t>(t.Hash()));
}


/// 128-bit digest = two independent FNV lanes over the term list
/// (ADR §4.2.1). `enabled` and `seed` are NOT digest terms; only the
/// topology-class parameter values (02 §6) enter, together with type,
/// algorithmVersion, mode, resolved space/readPhase, sorted input paths,
/// sorted reference/map/surface relationship targets, and the children's
/// digests.
UsdGenEpoch ComputeNodeDigest(
    UsdGenNodeDesc const &nd,
    TfToken const &type,
    int algorithmVersion,
    UsdGenSpace space,
    UsdGenReadPhase readPhase,
    TfSpan<const TfToken> topoParams,
    std::vector<SdfPath> const &inputPaths,
    std::vector<SdfPath> const &refPaths,
    std::vector<std::pair<SdfPath, UsdGenEpoch>> const &childDigests)
{
    uint64_t h0 = 1469598103934665603ULL;
    uint64_t h1 = 1469598103934665603ULL;
    auto mix = [&h0, &h1](uint64_t v) {
        h0 = Fnv1aI64(h0, static_cast<int64_t>(v));
        h1 = Fnv1aI64(h1, static_cast<int64_t>(~v));
    };

    mix(0xa5a5a5a5ULL ^ Fnv1aCstr(0, type.GetText()));
    mix(0x00010001ULL ^ Fnv1aI64(0, algorithmVersion));
    mix(0x00020002ULL ^ Fnv1aTfToken(0, nd.mode));
    mix(0x00030003ULL ^ static_cast<uint64_t>(space));
    mix(0x00040004ULL ^ static_cast<uint64_t>(readPhase));
    for (SdfPath const &p : inputPaths) mix(0x00050005ULL ^ Fnv1aCstr(0, p.GetText()));
    for (SdfPath const &p : refPaths)   mix(0x00060006ULL ^ Fnv1aCstr(0, p.GetText()));
    for (auto const &param : nd.params) {
        for (TfToken const &t : topoParams) {
            if (param.name == t) {
                mix(0x00070007ULL ^ Fnv1aTfToken(0, t));
                mix(0x00080008ULL ^ param.value.GetHash());
                break;
            }
        }
    }
    for (SdfPath const &p : inputPaths) {
        for (auto const &kv : childDigests)
            if (kv.first == p) { mix(kv.second[0] ^ (kv.second[1] << 1)); break; }
    }
    return {h0, h1};
}

// 02-schema.md §6 dirty classification, per (type, param). The §6.1
// graph-structural rows are DIGEST terms (a change recompiles the node);
// the §6.2 topology rows and the §6.4 capture rows are not.

// Interned once: E-6 measures the WHOLE-node walk (every routed param of
// every node), so the classification tables must compare token POINTERS,
// not re-intern a `TfToken("literal")` per comparison.
namespace tok {
const TfToken &T() { static const TfToken t{"input"}; return t; }
const TfToken &AlgorithmVersion() { static const TfToken t{"algorithmVersion"}; return t; }
const TfToken &Space() { static const TfToken t{"space"}; return t; }
const TfToken &ReadPhase() { static const TfToken t{"readPhase"}; return t; }
const TfToken &Guides() { static const TfToken t{"guides"}; return t; }
const TfToken &Curves() { static const TfToken t{"curves"}; return t; }
const TfToken &Surface() { static const TfToken t{"surface"}; return t; }
const TfToken &Enabled() { static const TfToken t{"enabled"}; return t; }
const TfToken &Seed() { static const TfToken t{"seed"}; return t; }
const TfToken &MaskSource() { static const TfToken t{"mask:source"}; return t; }
const TfToken &MaskCombine() { static const TfToken t{"mask:combine"}; return t; }
const TfToken &MaskRangeMode() { static const TfToken t{"mask:rangeMode"}; return t; }
const TfToken &Mode() { static const TfToken t{"mode"}; return t; }
const TfToken &Segments() { static const TfToken t{"segments"}; return t; }
const TfToken &Direction() { static const TfToken t{"direction"}; return t; }
const TfToken &LengthSource() { static const TfToken t{"length:source"}; return t; }
const TfToken &LengthMethod() { static const TfToken t{"length:method"}; return t; }
const TfToken &Rebuild() { static const TfToken t{"rebuild"}; return t; }
const TfToken &Replace() { static const TfToken t{"replace"}; return t; }
const TfToken &LengthMode() { static const TfToken t{"length:mode"}; return t; }
const TfToken &CullThreshold() { static const TfToken t{"cullThreshold"}; return t; }
const TfToken &Scatter() { static const TfToken t{"UsdGenScatter"}; return t; }
const TfToken &Grow() { static const TfToken t{"UsdGenGrow"}; return t; }
const TfToken &Length() { static const TfToken t{"UsdGenLength"}; return t; }
const TfToken &Width() { static const TfToken t{"UsdGenWidth"}; return t; }
}  // namespace tok

/// §6.1: this parameter is a term of the node's Merkle structural digest
/// (recompile on edit).
bool IsDigestParam(TfToken const &type, TfToken const &param)
{
    // Universal structural rows (02 §6.1).
    if (param == tok::MaskSource() || param == tok::MaskCombine() ||
        param == tok::MaskRangeMode())
        return true;
    if (type == tok::Scatter())
        return param == tok::Mode() || param == tok::Guides();
    if (type == tok::Grow())
        return param == tok::Segments() || param == tok::Direction() ||
               param == tok::LengthSource();
    if (type == tok::Length())
        return param == tok::LengthMethod() || param == tok::Rebuild() ||
               param == tok::LengthSource();
    if (type == tok::Width())
        return param == tok::Replace();
    return false;
}

/// §6.2: topology-class rows that re-allocate without touching the digest.
bool IsTopologyParam(TfToken const &type, TfToken const &param)
{
    if (type == tok::Length())
        return param == tok::LengthMode() || param == tok::CullThreshold();
    return false;
}

/// Classify one authored parameter into UsdGenDirtyBits (02 §6 row table).
/// `inTopoList` states that the caller already KNOWS the param is one of
/// op.TopologyParameters() — the membership tail loops are the O(n^2) term
/// of building every node's routing table and are pure waste when the
/// caller iterates that very list.
uint32_t ClassifyParamBits(
    UsdGenOp const &op, TfToken const &type, TfToken const &param,
    UsdGenTopoFx topoFx, bool inTopoList = false)
{
    if (param == tok::T() || param == tok::AlgorithmVersion() ||
        param == tok::Space() || param == tok::ReadPhase()) {
        return UsdGenDirtyStructural;
    }
    // Relationship retargets are graph-structural (02 §6.1): they change a
    // graph edge / the digest's relationship list.
    if (param == tok::Guides() || param == tok::Curves()) {
        return UsdGenDirtyStructural;
    }
    if (param == tok::Surface()) {
        return UsdGenDirtyStructural;  // bound-surface retarget: recompile
    }
    if (param == tok::Enabled()) {
        // 02 §6.2/§6.3: topology for generators and Length (its static
        // TopologyEffect() is CurveCount), value-toggle for the rest.
        return (topoFx != UsdGenTopoFx::None) ? UsdGenDirtyTopology
                                              : UsdGenDirtyParameter;
    }
    if (param == tok::Seed()) {
        return UsdGenDirtyCapture;
    }
    if (IsDigestParam(type, param)) {
        return UsdGenDirtyStructural;
    }
    if (IsTopologyParam(type, param)) {
        return UsdGenDirtyTopology;
    }
    if (inTopoList) return UsdGenDirtyCapture;       // §6.4 capture-class edit
    for (TfToken const &t : op.TopologyParameters()) {
        if (t == param) return UsdGenDirtyCapture;   // §6.4 capture-class edit
    }
    for (TfToken const &t : op.ValueParameters()) {
        if (t == param) return UsdGenDirtyParameter;
    }
    // Unknown usdGen:* leaf: value-dirty is the safe superset (02 §6).
    return UsdGenDirtyParameter;
}

/// The (routing table, digest-param list) of an operator TYPE is a static
/// function of the type: build it ONCE per type, not once per node. E-6
/// walks 200+ nodes in one recompile; without this cache the classification
/// is O(nodes x params x param-list) per call.
struct TypeClassTable
{
    std::vector<std::pair<TfToken, uint32_t>> routing;  // paramRouting content
    TfTokenVector digestParams;   // TopologyParameters() ∩ §6.1 digest rows
};

TypeClassTable const &TypeClassification(UsdGenOp const &op)
{
    thread_local std::unordered_map<std::string, TypeClassTable> cache;
    TfToken const type = op.Type();
    auto it = cache.find(type.GetString());
    if (it != cache.end()) return it->second;

    TypeClassTable tbl;
    UsdGenTopoFx const topoFx = op.TopologyEffect();
    for (TfToken const &t : op.TopologyParameters()) {
        tbl.routing.push_back({t, ClassifyParamBits(op, type, t, topoFx, true)});
        if (IsDigestParam(type, t)) tbl.digestParams.push_back(t);
    }
    for (TfToken const &t : op.ValueParameters())
        tbl.routing.push_back({t, UsdGenDirtyParameter});
    return cache.emplace(type.GetString(), std::move(tbl)).first->second;
}

}  // namespace

// ---------------------------------------------------------------------------

UsdGenCompileResult UsdGenCompiler::Compile(UsdGenGraphDesc const &desc, UsdGenGraph *out)
{
    UsdGenCompileResult result;
    _Build(desc, out, /*reuse=*/nullptr, result);
    if (result.errors.empty()) {
        result.ok = true;
        // Fresh compile: every node re-captures; all chunks value-dirty so
        // the first commit evaluates the whole chain.
        for (auto &np : out->_nodes) {
            np->captureNeeded = true;
            std::fill(np->chunkDirty.begin(), np->chunkDirty.end(),
                      UsdGenDirtyParameter);
        }
    }
    return result;
}

UsdGenCompileResult UsdGenCompiler::Recompile(UsdGenGraphDesc const &newDesc, UsdGenGraph *out)
{
    UsdGenCompileResult result;
    _Build(newDesc, out, out, result);
    if (result.errors.empty()) {
        result.ok = true;
        // Rebuilt nodes re-capture; digest-stable nodes keep their runtime
        // state (capture, buffer, chunk + dirty bytes) moved over by
        // _Build. The runtime partition is re-established by the first
        // commit once a generator installs its topology — a recompile
        // cannot know the live curve count of an unpopulated output, so it
        // must not call Repartition() here (that would wipe the moved
        // chunk/dirty state and capture flags).
        for (auto &np : out->_nodes) np->captureNeeded = false;
        for (UsdGenNodeId id : result.rebuilt) out->Node(id).captureNeeded = true;
    }
    return result;
}

void UsdGenCompiler::_Build(
    UsdGenGraphDesc const &desc,
    UsdGenGraph *out,
    UsdGenGraph const *reuse,
    UsdGenCompileResult &result)
{
    usdGenRegisterM1Operators();   // idempotent

    if (desc.nodes.empty()) {
        result.errors.push_back("UsdGenCompiler: graph has no operator nodes");
        return;
    }
    if (desc.terminal.IsEmpty()) {
        result.errors.push_back("UsdGenCompiler: no usdGen:terminal target");
        return;
    }

    // desc index by path (namespace order == desc.nodes' order, S26).
    // Sorted vector, not unordered_map (E-6): one allocation + sort instead
    // of ~200 per-insert map-node mallocs; binary search per lookup.
    // SdfPath::operator< orders by full path.
    std::vector<std::pair<SdfPath, int>> descIdxByPath;
    descIdxByPath.reserve(desc.nodes.size());
    for (size_t i = 0; i < desc.nodes.size(); ++i) {
        descIdxByPath.emplace_back(desc.nodes[i].path, static_cast<int>(i));
    }
    std::sort(descIdxByPath.begin(), descIdxByPath.end(),
              [](auto const &a, auto const &b) { return a.first < b.first; });
    auto findDescIdx = [&](SdfPath const &p) {
        auto it = std::lower_bound(
            descIdxByPath.begin(), descIdxByPath.end(), p,
            [](std::pair<SdfPath, int> const &e, SdfPath const &v) { return e.first < v; });
        return (it != descIdxByPath.end() && !(p < it->first)) ? it
            : descIdxByPath.end();
    };
    auto termIt = findDescIdx(desc.terminal);
    if (termIt == descIdxByPath.end()) {
        result.errors.push_back(
            std::string("UsdGenCompiler: usdGen:terminal target '") +
            desc.terminal.GetText() + "' is not an operator prim in the graph");
        return;
    }

    int const n = static_cast<int>(desc.nodes.size());
    // Edge resolution, fused single pass (E-6): every input's desc index is
    // looked up ONCE into flat arrays shared by the counters, the CSR fill
    // and Kahn — not re-searched per pass.
    std::vector<int> edgeOff(size_t(n) + 1, 0);
    for (int i = 0; i < n; ++i)
        edgeOff[size_t(i) + 1] = edgeOff[size_t(i)] + int(desc.nodes[i].inputs.size());
    std::vector<int> edgeIds;
    edgeIds.assign(size_t(edgeOff[size_t(n)]), -1);
    std::vector<int> inDeg(n, 0);
    for (int i = 0; i < n; ++i) {
        for (int k = edgeOff[size_t(i)]; k < edgeOff[size_t(i) + 1]; ++k) {
            SdfPath const &in = desc.nodes[i].inputs[size_t(k - edgeOff[size_t(i)])];
            auto it = findDescIdx(in);
            if (it == descIdxByPath.end()) {
                result.errors.push_back(
                    std::string("UsdGenCompiler: usdGen:input target '") + in.GetText() +
                    "' of '" + desc.nodes[i].path.GetText() + "' is not in the graph");
                return;
            }
            edgeIds[size_t(k)] = it->second;
            ++inDeg[i];
        }
    }
    // Consumers in CSR form (E-6): one allocation instead of n per-node
    // vectors. consumersOf[i] = [begin,end) into consumerIds.
    std::vector<int> consumersOf(size_t(n) + 1, 0);
    for (int id : edgeIds) ++consumersOf[size_t(id) + 1];
    for (int i = 0; i < n; ++i) consumersOf[size_t(i) + 1] += consumersOf[size_t(i)];
    std::vector<int> consumerIds;
    consumerIds.resize(size_t(consumersOf[size_t(n)]));
    {
        std::vector<int> cursor(consumersOf.begin(), consumersOf.begin() + n);
        for (int i = 0; i < n; ++i)
            for (int k = edgeOff[size_t(i)]; k < edgeOff[size_t(i) + 1]; ++k)
                consumerIds[size_t(cursor[edgeIds[size_t(k)]]++)] = i;
    }

    // Kahn with namespace-order tie-break: among the ready nodes, always
    // pick the lowest desc index (02 §3.2 dense ids). Min-heap, not std::set:
    // identical order, zero per-node allocations (E-6: 200 tree-node mallocs).
    std::vector<int> order;
    order.reserve(n);
    {
        std::vector<int> ready;
        ready.reserve(n);
        for (int i = 0; i < n; ++i) if (inDeg[i] == 0) ready.push_back(i);
        std::make_heap(ready.begin(), ready.end(), std::greater<int>());
        while (!ready.empty()) {
            std::pop_heap(ready.begin(), ready.end(), std::greater<int>());
            int const i = ready.back();
            ready.pop_back();
            order.push_back(i);
            for (int k = consumersOf[size_t(i)]; k < consumersOf[size_t(i) + 1]; ++k) {
                int const j = consumerIds[size_t(k)];
                if (--inDeg[j] == 0) {
                    ready.push_back(j);
                    std::push_heap(ready.begin(), ready.end(), std::greater<int>());
                }
            }
        }
    }
    if (static_cast<int>(order.size()) != n) {
        for (int i = 0; i < n; ++i) {
            if (inDeg[i] > 0) {
                result.errors.push_back(
                    std::string("UsdGenCompiler: input cycle involving '") +
                    desc.nodes[i].path.GetText() + "'");
                break;
            }
        }
        return;
    }
    // Dense node ids: topo position == id.
    std::vector<int> topoOfDesc(n, -1);
    for (int pos = 0; pos < n; ++pos) topoOfDesc[order[pos]] = pos;

    UsdGenNodeId const terminalId =
        static_cast<UsdGenNodeId>(topoOfDesc[termIt->second]);

    // Strict descendants as dense-id bitmaps propagated in REVERSE
    // topological order — O(edges * words) word-ORs. This replaces a
    // per-node std::set BFS whose O(n^2) allocations were the dominant
    // term of the E-6 recompile budget on a 200-node chain.
    // M-4: the DIRECT consumer is a descendant too.
    std::vector<std::vector<UsdGenNodeId>> descOf;
    descOf.resize(size_t(n));
    {
        int const words = (n + 63) / 64;
        std::vector<uint64_t> bits(size_t(n) * words, 0);
        // Counts per row first, so every row allocates exactly once below.
        std::vector<int> rowCount(size_t(n), 0);
        for (int pos = n - 1; pos >= 0; --pos) {
            int const di = order[pos];
            uint64_t *dst = bits.data() + size_t(pos) * words;
            for (int k = consumersOf[size_t(di)]; k < consumersOf[size_t(di) + 1]; ++k) {
                int const c = consumerIds[size_t(k)];
                int const cp = topoOfDesc[c];   // cp > pos (topological)
                uint64_t const *src = bits.data() + size_t(cp) * words;
                // OR + popcount in one row pass (E-6): the count feeds the
                // exact reserve below, so rows never regrow.
                int add = 0;
                for (int w = 0; w < words; ++w) {
                    uint64_t const before = dst[w];
                    uint64_t const after = before | src[w];
                    dst[w] = after;
                    add += __builtin_popcountll(after & ~before);
                }
                uint64_t const bit = 1ull << (cp & 63);
                if (!(dst[cp >> 6] & bit)) { dst[cp >> 6] |= bit; ++add; }
                rowCount[size_t(pos)] += add;
            }
        }
        for (int pos = 0; pos < n; ++pos) {
            uint64_t const *row = bits.data() + size_t(pos) * words;
            auto &v = descOf[size_t(pos)];
            v.reserve(size_t(rowCount[size_t(pos)]));
            for (int w = 0; w < words; ++w) {
                uint64_t b = row[w];
                while (b) {
                    v.push_back(static_cast<UsdGenNodeId>(
                        w * 64 + __builtin_ctzll(b)));
                    b &= b - 1;   // ascending bit order == dense-id order
                }
            }
        }
    }
    // Reuse matching (E-6): index the previous graph's nodes by path WITHOUT
    // copying anything (no Clone, no buffer/vector copies). Stable nodes are
    // moved whole — op, capture, buffer, chunks, dirty bytes, epochs — into
    // the new graph; only their desc pointers and digests refresh. This
    // replaces the OldNode snapshot whose per-node Clone + deep copies were
    // the dominant term of the chain-200 append budget.
    std::vector<std::unique_ptr<UsdGenCompiledNode>> oldNodes;
    // descIdx → node index over the OLD graph (E-6): the merge and the node
    // loop map by position, with a linear path scan only for reorder misses
    // (rare) — no index vector, no sort, no per-lookup searches on the hit
    // path. SdfPath::operator== is an integer compare (no strings).
    std::vector<size_t> nodeByDesc;
    if (reuse && reuse != out) {
        // Defensive: Recompile always passes out as reuse; a foreign graph
        // is indexed read-only (no moves) — nodes rebuild fully below.
    } else if (reuse) {
        oldNodes = std::move(out->_nodes);
    }
    if (!oldNodes.empty() && out->_desc) {
        nodeByDesc.assign(out->_desc->nodes.size(), SIZE_MAX);
        for (size_t i = 0; i < oldNodes.size(); ++i)
            if (oldNodes[i] && oldNodes[i]->descIdx >= 0 &&
                size_t(oldNodes[i]->descIdx) < nodeByDesc.size())
                nodeByDesc[size_t(oldNodes[i]->descIdx)] = i;
    }
    // Desc copy elision (E-6): node entries byte-identical to the previous
    // graph's copy move instead of copying — a 201-entry copy is ~600 small
    // allocs (vectors + VtValue holders per param). Scalars always copy.
    // Entry identity is by path; equality is field-wise (no allocs on hit).
    // Correctness: nodes read their desc entry read-only through desc/paramView.
    auto sameNodeDesc = [](UsdGenNodeDesc const &a, UsdGenNodeDesc const &b) {
        if (a.path != b.path || a.type != b.type || a.mode != b.mode ||
            a.algorithmVersion != b.algorithmVersion || a.enabled != b.enabled ||
            a.seed != b.seed || a.blend != b.blend || a.space != b.space ||
            a.readPhase != b.readPhase || a.inputs != b.inputs ||
            a.references != b.references || a.curves != b.curves ||
            a.surfaces != b.surfaces || a.maps != b.maps ||
            a.params.size() != b.params.size() || a.ramps.size() != b.ramps.size())
            return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (a.params[i].name != b.params[i].name ||
                a.params[i].animated != b.params[i].animated ||
                !(a.params[i].value == b.params[i].value))
                return false;
        }
        for (size_t i = 0; i < a.ramps.size(); ++i) {
            if (a.ramps[i].knots != b.ramps[i].knots ||
                a.ramps[i].positions != b.ramps[i].positions ||
                a.ramps[i].colors != b.ramps[i].colors ||
                a.ramps[i].interpolation != b.ramps[i].interpolation)
                return false;
        }
        return true;
    };
    UsdGenGraphDesc *oldDescPtr = (reuse && out->_desc) ? out->_desc.get() : nullptr;
    std::vector<char> descChanged(desc.nodes.size(), 1);
    // oldNodeForNewDesc[di] = old node index, or -1 (recorded by the merge,
    // so the node loop needs no per-node searches on the hit path).
    std::vector<int> oldNodeForNewDesc(desc.nodes.size(), -1);
    // entries copy from the input. Entry ORDER follows the input (S26).
    // Identity is positional (nodeByDesc) with a linear-scan fallback.
    static_assert(sizeof(UsdGenGraphDesc) == 488,
        "UsdGenGraphDesc changed size: update the Recompile shell merge below");
    {
        auto fresh = std::make_unique<UsdGenGraphDesc>();
        fresh->description = desc.description;
        fresh->terminal = desc.terminal;
        fresh->curveSets = desc.curveSets;
        fresh->surfaces = desc.surfaces;
        fresh->maps = desc.maps;
        fresh->look = desc.look;
        fresh->xformMatrix = desc.xformMatrix;
        fresh->purpose = desc.purpose;
        fresh->visibility = desc.visibility;
        fresh->materialPath = desc.materialPath;
        fresh->pickTarget = desc.pickTarget;
        fresh->densityScale = desc.densityScale;
        fresh->renderDensityScale = desc.renderDensityScale;
        fresh->tileTarget = desc.tileTarget;
        fresh->curveBasis = desc.curveBasis;
        fresh->motionMode = desc.motionMode;
        fresh->motionSampleCount = desc.motionSampleCount;
        fresh->forwardSurfaceSamples = desc.forwardSurfaceSamples;
        fresh->schemaVersion = desc.schemaVersion;
        fresh->time = desc.time;
        fresh->nodes.reserve(desc.nodes.size());
        // Per-input-order entry change flags for digest-change propagation
        // below (plan §3.5: only changed nodes and their descendants pay).
        if (oldDescPtr) {
            size_t const oldSize = oldDescPtr->nodes.size();
            for (size_t i = 0; i < desc.nodes.size(); ++i) {
                // Positional pre-check (E-6): appends/param-edits keep
                // namespace order — one pointer-fast SdfPath == instead of
                // a binary search on the hit path.
                size_t o = oldSize;  // sentinel: linear-scan fallback below
                size_t on = oldSize; // old NODE index for oldNodeForNewDesc
                if (i < oldSize && oldDescPtr->nodes[i].path == desc.nodes[i].path) {
                    o = i;
                    if (i < nodeByDesc.size() && nodeByDesc[i] != SIZE_MAX) on = nodeByDesc[i];
                } else {
                    // Reorder miss (rare): linear path scan with integer
                    // SdfPath compares; node index rides nodeByDesc.
                    for (size_t j = 0; j < oldSize; ++j) {
                        if (oldDescPtr->nodes[j].path == desc.nodes[i].path) {
                            o = j;
                            if (j < nodeByDesc.size() && nodeByDesc[j] != SIZE_MAX)
                                on = nodeByDesc[j];
                            break;
                        }
                    }
                }
                if (o < oldSize && sameNodeDesc(oldDescPtr->nodes[o], desc.nodes[i])) {
                    fresh->nodes.push_back(std::move(oldDescPtr->nodes[o]));
                    descChanged[i] = 0;
                    if (on < oldSize) oldNodeForNewDesc[i] = int(on);
                } else {
                    fresh->nodes.push_back(desc.nodes[i]);
                }
            }
        } else {
            fresh->nodes = desc.nodes;
        }
        out->_desc = std::move(fresh);
    }
    out->_nodes.clear();
    out->_nodes.resize(n);
    out->_nodeByPath.clear();
    out->_terminal = terminalId;
    out->_chunkSize = kUsdGenDefaultChunkSize;   // USDGEN_CHUNK_SIZE (S23)
    out->_tileTarget = desc.tileTarget;
    std::vector<UsdGenEpoch> digests(n);
    std::vector<char> digestChanged(n, 0);
    for (int pos = 0; pos < n; ++pos) {
        int const di = order[pos];
        UsdGenNodeDesc const &nd = desc.nodes[di];
        // Change propagation (plan §3.5): a node whose desc entry is
        // byte-identical AND whose inputs' digests all survived keeps its
        // old digest verbatim — no FNV, no path vectors, no GetHash. Only
        // changed nodes and downstream consumers recompute.
        // Inputs come pre-resolved from the fused edge pass (E-6): no
        // per-node binary searches for the change check or the refill.
        int const e0 = edgeOff[size_t(di)], e1 = edgeOff[size_t(di) + 1];
        bool inputChanged = false;
        for (int k = e0; !inputChanged && k < e1; ++k)
            inputChanged = digestChanged[topoOfDesc[edgeIds[size_t(k)]]] != 0;
        if (!oldNodes.empty() && !descChanged[di] && !inputChanged &&
            oldNodeForNewDesc[di] >= 0) {
            UsdGenCompiledNode &oldS = *oldNodes[size_t(oldNodeForNewDesc[di])];
            if (oldS.type == nd.type &&
                oldS.algorithmVersion == nd.algorithmVersion) {
                // Verbatim reuse: refresh desc-owned fields, recompute the
                // cheap value digest, rebuild the dense-id edge list in
                // retained capacity (no alloc when the fan-in is stable).
                auto moved = std::move(oldNodes[size_t(oldNodeForNewDesc[di])]);
                moved->id = static_cast<UsdGenNodeId>(pos);
                moved->desc = &out->_desc->nodes[di];
                moved->descIdx = di;
                moved->enabled = nd.enabled;
                moved->paramView.desc = out->_desc.get();
                moved->paramView.node = &out->_desc->nodes[di];
                moved->inputs.clear();
                for (int k = e0; k < e1; ++k)
                    moved->inputs.push_back(static_cast<UsdGenNodeId>(topoOfDesc[edgeIds[size_t(k)]]));
                std::sort(moved->inputs.begin(), moved->inputs.end());
                moved->input = moved->inputs.empty() ? kUsdGenInvalidNode : moved->inputs.front();
                moved->descendants = std::move(descOf[size_t(pos)]);
                {   // value digest refreshes (cheap, no allocs).
                    uint64_t vh = 1469598103934665603ULL;
                    auto feed = [&vh](uint64_t v) { vh ^= v; vh *= 0x100000001b3ULL; };
                    feed(static_cast<uint64_t>(nd.enabled));
                    for (auto const &param : nd.params) {
                        bool inValue = false, inTopo = false;
                        for (TfToken const &t : moved->op->ValueParameters())
                            inValue = inValue || (t == param.name);
                        for (TfToken const &t : moved->op->TopologyParameters())
                            inTopo = inTopo || (t == param.name);
                        if (inValue || (!inTopo && param.name != tok::Seed())) feed(param.value.GetHash());
                    }
                    moved->paramValueDigest = vh;
                }
                digests[pos] = moved->structuralDigest;
                out->_nodeByPath[nd.path] = moved->id;
                out->_nodes[pos] = std::move(moved);
                continue;
            }
        }
        // E-6 fast path: a path-matched old node of the same type and
        // version reuses its OWN op for the stability check — no op Create,
        // no vector building, no routing copy. On stability the old node
        // moves whole with only desc pointers/digests refreshed; the
        // expensive per-node allocs (node, inputs, refs, routing table) and
        // their matching destructions never happen.
        if (!oldNodes.empty() && oldNodeForNewDesc[di] >= 0) {
            UsdGenCompiledNode &old0 = *oldNodes[size_t(oldNodeForNewDesc[di])];
            if (old0.type == nd.type &&
                old0.algorithmVersion == nd.algorithmVersion) {
                UsdGenSpace sp = UsdGenSpace::Inherit;
                if (nd.space == TfToken("rest")) sp = UsdGenSpace::Rest;
                else if (nd.space == TfToken("deformed")) sp = UsdGenSpace::Deformed;
                UsdGenReadPhase rp = UsdGenReadPhase::Final;
                if (nd.readPhase == TfToken("base")) rp = UsdGenReadPhase::Base;
                TypeClassTable const &tbl0 = TypeClassification(*old0.op);
                std::vector<SdfPath> inputPaths0 = nd.inputs;
                std::sort(inputPaths0.begin(), inputPaths0.end());
                std::vector<SdfPath> refPaths0 = nd.references;
                refPaths0.insert(refPaths0.end(), nd.maps.begin(), nd.maps.end());
                refPaths0.insert(refPaths0.end(), nd.surfaces.begin(), nd.surfaces.end());
                std::sort(refPaths0.begin(), refPaths0.end());
                std::vector<std::pair<SdfPath, UsdGenEpoch>> childDigests0;
                for (SdfPath const &p : inputPaths0) {
                    auto const it = findDescIdx(p);
                    if (it != descIdxByPath.end())
                        childDigests0.emplace_back(p, digests[topoOfDesc[it->second]]);
                }
                UsdGenEpoch const dg0 = ComputeNodeDigest(
                    nd, nd.type, old0.algorithmVersion, sp, rp,
                    TfSpan<const TfToken>(tbl0.digestParams.data(), tbl0.digestParams.size()),
                    inputPaths0, refPaths0, childDigests0);
                if (dg0 == old0.structuralDigest &&
                    old0.space == sp && old0.readPhase == rp &&
                    old0.topoFx == old0.op->TopologyEffect() &&
                    old0.role == old0.op->Role()) {
                    // Stable: move whole, refresh desc-owned fields only.
                    // inputs/descendants-aside everything rides along:
                    // op, capture, buffer, chunks, dirty bytes, epochs,
                    // lastParamDigest (scheduler skip state survives).
                    auto moved = std::move(oldNodes[size_t(oldNodeForNewDesc[di])]);
                    moved->id = static_cast<UsdGenNodeId>(pos);
                    moved->desc = &out->_desc->nodes[di];
                    moved->descIdx = di;
                    moved->structuralDigest = dg0;
                    moved->enabled = nd.enabled;
                    moved->paramView.desc = out->_desc.get();
                    moved->paramView.node = &out->_desc->nodes[di];
                    moved->descendants = std::move(descOf[size_t(pos)]);
                    {   // value digest refreshes (cheap, no allocs).
                        uint64_t vh = 1469598103934665603ULL;
                        auto feed = [&vh](uint64_t v) { vh ^= v; vh *= 0x100000001b3ULL; };
                        feed(static_cast<uint64_t>(nd.enabled));
                        for (auto const &param : nd.params) {
                            bool inValue = false, inTopo = false;
                            for (TfToken const &t : moved->op->ValueParameters())
                                inValue = inValue || (t == param.name);
                            for (TfToken const &t : moved->op->TopologyParameters())
                                inTopo = inTopo || (t == param.name);
                            if (inValue || (!inTopo && param.name != tok::Seed())) feed(param.value.GetHash());
                        }
                        moved->paramValueDigest = vh;
                    }
                    digests[pos] = dg0;
                    digestChanged[pos] = 0;
                    out->_nodeByPath[nd.path] = moved->id;
                    out->_nodes[pos] = std::move(moved);
                    continue;
                }
            }
        }

        auto node = std::make_unique<UsdGenCompiledNode>();
        node->id = static_cast<UsdGenNodeId>(pos);
        node->type = nd.type;
        node->desc = &out->_desc->nodes[di];
        node->descIdx = di;

        int resolvedAlgo = nd.algorithmVersion;
        std::unique_ptr<UsdGenOp> op =
            UsdGenOpRegistry::Get().Create(nd.type, resolvedAlgo, &resolvedAlgo);
        if (!op) {
            result.errors.push_back(
                "UsdGenCompiler: no kernel registered for '" +
                nd.type.GetString() + "' (prim " + nd.path.GetText() + ")");
            return;
        }
        node->op = std::move(op);
        node->algorithmVersion = resolvedAlgo;

        // space (S25): "auto" -> the type's Space(); else the authored value.
        node->space = UsdGenSpace::Inherit;
        if (nd.space == TfToken("rest")) node->space = UsdGenSpace::Rest;
        else if (nd.space == TfToken("deformed")) node->space = UsdGenSpace::Deformed;
        // readPhase (R9): "preceding" aliases "final".
        if (nd.readPhase == TfToken("base")) node->readPhase = UsdGenReadPhase::Base;
        else if (nd.readPhase == TfToken("preceding")) {
            result.warnings.push_back(
                std::string("usdGen:readPhase 'preceding' is an alias for 'final' on ") +
                nd.path.GetText());
            node->readPhase = UsdGenReadPhase::Final;
        } else {
            node->readPhase = UsdGenReadPhase::Final;
        }
        node->topoFx = node->op->TopologyEffect();
        node->role = node->op->Role();
        node->enabled = nd.enabled;

        // Input edges (dense ids).
        for (SdfPath const &in : nd.inputs) {
            auto const it = findDescIdx(in);
            if (it != descIdxByPath.end())
                node->inputs.push_back(static_cast<UsdGenNodeId>(topoOfDesc[it->second]));
        }
        std::sort(node->inputs.begin(), node->inputs.end());
        node->input = node->inputs.empty() ? kUsdGenInvalidNode : node->inputs.front();

        // Parameter view over the graph's desc copy.
        node->paramView.desc = out->_desc.get();
        node->paramView.node = &out->_desc->nodes[di];

        // Relationship targets.
        node->curveRefs = nd.curves;
        node->mapRefs = nd.maps;
        if (!nd.surfaces.empty()) {
            node->hasSurface = true;
            node->surface = 0;   // filled below when desc.surfaces is indexed
        }

        // Strict descendants, ascending dense-id order (bitmap pass above).
        node->descendants = std::move(descOf[size_t(pos)]);

        // Structural digest (ADR §4.2.1 term list).
        std::vector<SdfPath> inputPaths = nd.inputs;
        std::sort(inputPaths.begin(), inputPaths.end());
        std::vector<SdfPath> refPaths = nd.references;
        refPaths.insert(refPaths.end(), nd.maps.begin(), nd.maps.end());
        refPaths.insert(refPaths.end(), nd.surfaces.begin(), nd.surfaces.end());
        std::sort(refPaths.begin(), refPaths.end());
        std::vector<std::pair<SdfPath, UsdGenEpoch>> childDigests;
        for (SdfPath const &p : inputPaths) {
            auto const it = findDescIdx(p);
            if (it != descIdxByPath.end())
                childDigests.emplace_back(p, digests[topoOfDesc[it->second]]);
        }
        // Structural digest: only §6.1-class (recompile) parameter values
        // are digest terms — capture- and topology-class edits must NOT
        // move the digest (03 §3.3/§3.6). The §6.1 ∩ TopologyParameters()
        // filter and the routing table are properties of the TYPE and are
        // built once per type, not once per node (E-6 budget).
        TypeClassTable const &tbl = TypeClassification(*node->op);
        digests[pos] = ComputeNodeDigest(
            nd, node->type, node->algorithmVersion, node->space, node->readPhase,
            TfSpan<const TfToken>(tbl.digestParams.data(), tbl.digestParams.size()),
            inputPaths, refPaths, childDigests);
        node->structuralDigest = digests[pos];

        // Value-class digest (skip-signature term, 03 §3.2): recompile keeps
        // this stable unless a value-class param or the enabled flag moved.
        // Unknown (unclassified) params feed it too — a value-class edit is
        // the safe superset for an unlisted property.
        {
            uint64_t vh = 1469598103934665603ULL;
            auto feed = [&vh](uint64_t v) { vh ^= v; vh *= 0x100000001b3ULL; };
            feed(static_cast<uint64_t>(nd.enabled));
            for (auto const &param : nd.params) {
                bool inValue = false, inTopo = false;
                for (TfToken const &t : node->op->ValueParameters())
                    inValue = inValue || (t == param.name);
                for (TfToken const &t : node->op->TopologyParameters())
                    inTopo = inTopo || (t == param.name);
                if (inValue || (!inTopo && param.name != tok::Seed())) feed(param.value.GetHash());
            }
            node->paramValueDigest = vh;
        }

        // Parameter routing table (02 §6): every C1 parameter of this type
        // gets exactly one entry — copied from the cached type table.
        node->paramRouting = tbl.routing;

        // E-6 reuse: a digest-STABLE node moves whole — op, capture, buffer,
        // chunks, dirty bytes, epochs — out of the previous graph. Only the
        // desc pointers and digests refresh. A null capture (never-run node)
        // is NOT a rebuild: the capture installs lazily on the next commit.
        if (!oldNodes.empty() && oldNodeForNewDesc[di] >= 0) {
            UsdGenCompiledNode const &oldRef = *oldNodes[size_t(oldNodeForNewDesc[di])];
            bool const stable =
                oldRef.structuralDigest == node->structuralDigest &&
                oldRef.type == node->type &&
                oldRef.algorithmVersion == node->algorithmVersion &&
                oldRef.space == node->space &&
                oldRef.readPhase == node->readPhase &&
                oldRef.topoFx == node->topoFx &&
                oldRef.role == node->role;
            if (stable) {
                // Move the whole node; refresh only what the new desc
                // owns. Op, capture, buffer, chunks, dirty bytes,
                // capture epoch, topologySeq and eval signature ride
                // along untouched — including lastParamDigest, so the
                // scheduler's skip logic survives the recompile.
                auto moved = std::move(oldNodes[size_t(oldNodeForNewDesc[di])]);
                moved->id = node->id;
                moved->desc = node->desc;
                moved->descIdx = node->descIdx;
                moved->structuralDigest = node->structuralDigest;
                moved->paramValueDigest = node->paramValueDigest;
                moved->enabled = node->enabled;
                moved->input = node->input;
                moved->inputs = std::move(node->inputs);
                moved->descendants = std::move(node->descendants);
                moved->curveRefs = std::move(node->curveRefs);
                moved->mapRefs = std::move(node->mapRefs);
                moved->hasSurface = node->hasSurface;
                moved->surface = node->surface;
                moved->paramView = node->paramView;
                moved->paramRouting = std::move(node->paramRouting);
                node = std::move(moved);
                digestChanged[pos] = 0;
            } else {
                result.rebuilt.push_back(node->id);
                digestChanged[pos] = 1;
            }
        } else {
            result.rebuilt.push_back(node->id);
            digestChanged[pos] = 1;
        }

        out->_nodeByPath[nd.path] = node->id;
        out->_nodes[pos] = std::move(node);
    }

    // Surface id assignment: index desc.surfaces; bind node->surface.
    std::unordered_map<SdfPath, UsdGenSurfaceId, SdfPath::Hash> surfaceIdByPath;
    for (size_t s = 0; s < out->_desc->surfaces.size(); ++s) {
        surfaceIdByPath[out->_desc->surfaces[s].path] =
            static_cast<UsdGenSurfaceId>(s);
        out->_desc->surfaces[s].id = static_cast<UsdGenSurfaceId>(s);
    }
    for (auto &np : out->_nodes) {
        if (!np->hasSurface) continue;
        auto const it = surfaceIdByPath.find(np->desc->surfaces.front());
        np->surface = (it != surfaceIdByPath.end()) ? it->second : 0;
    }
    result.structuralDigest = ComputeNodeDigest(
        out->_desc->nodes[termIt->second], TfToken("UsdGenTerminal"), 0,
        UsdGenSpace::Rest, UsdGenReadPhase::Final, {},
        {desc.terminal}, {},
        std::vector<std::pair<SdfPath, UsdGenEpoch>>{
            {desc.terminal, digests[terminalId]}});
}


}  // namespace usdGen
