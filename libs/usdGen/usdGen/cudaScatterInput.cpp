#include "usdGen/cudaScatterInput.h"

#include "usdGen/opRegistry.h"

#include "tbb/parallel_for.h"
#include "tbb/task_arena.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <vector>

namespace usdGen {
namespace {

CudaScatterInputStatus Fail(CudaScatterInputStatus status, std::string message,
                            std::string* reason) {
    if (reason) *reason = std::move(message);
    return status;
}

bool Finite(GfVec3f const& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool Finite(GfVec2f const& v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

// Scatter is a generator: it declares only density and flip.
bool AllowedScatterParam(TfToken const& name) {
    return name == TfToken("density") || name == TfToken("flip") || name == TfToken("subdivisionLevel");
}

bool IsFloat(VtValue const& v) { return v.IsHolding<float>() || v.IsHolding<double>(); }

// Assign-source iterator building float2/float3 values from K consecutive
// floats per step, so vector::assign copies into uninitialized storage:
// resize(n) would value-init (zero) the same elements assign then
// overwrites, and the non-scalar zeroing loop costs ~10% of the convert.
// GfVec2f/GfVec3f planes are contiguous floats (the static_asserts at the
// convert pin the sizes), so float-wise reads carry the exact bytes.
template <class Dst, int K>
struct FloatRunIterator {
    float const* p = nullptr;
    using iterator_category = std::random_access_iterator_tag;
    using value_type = Dst;
    using difference_type = std::ptrdiff_t;
    using pointer = Dst const*;
    using reference = Dst;
    FloatRunIterator() = default;
    explicit FloatRunIterator(float const* q) : p(q) {}
    Dst operator*() const;
    Dst operator[](difference_type i) const { return *(*this + i); }
    FloatRunIterator& operator++() { p += K; return *this; }
    FloatRunIterator operator++(int) { FloatRunIterator c(*this); p += K; return c; }
    FloatRunIterator& operator--() { p -= K; return *this; }
    FloatRunIterator operator--(int) { FloatRunIterator c(*this); p -= K; return c; }
    FloatRunIterator& operator+=(difference_type i) { p += K * i; return *this; }
    FloatRunIterator& operator-=(difference_type i) { p -= K * i; return *this; }
    friend FloatRunIterator operator+(FloatRunIterator a, difference_type i)
    {
        return a += i;
    }
    friend FloatRunIterator operator+(difference_type i, FloatRunIterator a)
    {
        return a += i;
    }
    friend FloatRunIterator operator-(FloatRunIterator a, difference_type i)
    {
        return a -= i;
    }
    friend difference_type operator-(FloatRunIterator const& a,
                                    FloatRunIterator const& b)
    {
        return (a.p - b.p) / K;
    }
    friend bool operator==(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p == b.p;
    }
    friend bool operator!=(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p != b.p;
    }
    friend bool operator<(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p < b.p;
    }
    friend bool operator<=(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p <= b.p;
    }
    friend bool operator>(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p > b.p;
    }
    friend bool operator>=(FloatRunIterator const& a, FloatRunIterator const& b)
    {
        return a.p >= b.p;
    }
};
template <>
inline float2 FloatRunIterator<float2, 2>::operator*() const
{
    return float2{p[0], p[1]};
}
template <>
inline float3 FloatRunIterator<float3, 3>::operator*() const
{
    return float3{p[0], p[1], p[2]};
}

// Assign-source iterator transposing the SoA point planes into float3
// positions: same no-zero-init rationale as FloatRunIterator.
struct TransposePositionsIterator {
    float const* px = nullptr;
    float const* py = nullptr;
    float const* pz = nullptr;
    using iterator_category = std::random_access_iterator_tag;
    using value_type = float3;
    using difference_type = std::ptrdiff_t;
    using pointer = float3 const*;
    using reference = float3;
    TransposePositionsIterator() = default;
    TransposePositionsIterator(float const* x, float const* y, float const* z)
        : px(x), py(y), pz(z) {}
    float3 operator*() const { return float3{px[0], py[0], pz[0]}; }
    float3 operator[](difference_type i) const { return *(*this + i); }
    TransposePositionsIterator& operator++()
    {
        ++px;
        ++py;
        ++pz;
        return *this;
    }
    TransposePositionsIterator operator++(int)
    {
        TransposePositionsIterator c(*this);
        ++*this;
        return c;
    }
    TransposePositionsIterator& operator--()
    {
        --px;
        --py;
        --pz;
        return *this;
    }
    TransposePositionsIterator operator--(int)
    {
        TransposePositionsIterator c(*this);
        --*this;
        return c;
    }
    TransposePositionsIterator& operator+=(difference_type i)
    {
        px += i;
        py += i;
        pz += i;
        return *this;
    }
    TransposePositionsIterator& operator-=(difference_type i)
    {
        px -= i;
        py -= i;
        pz -= i;
        return *this;
    }
    friend TransposePositionsIterator operator+(TransposePositionsIterator a,
                                               difference_type i)
    {
        return a += i;
    }
    friend TransposePositionsIterator operator+(difference_type i,
                                               TransposePositionsIterator a)
    {
        return a += i;
    }
    friend TransposePositionsIterator operator-(TransposePositionsIterator a,
                                               difference_type i)
    {
        return a -= i;
    }
    friend difference_type operator-(TransposePositionsIterator const& a,
                                    TransposePositionsIterator const& b)
    {
        return a.px - b.px;
    }
    friend bool operator==(TransposePositionsIterator const& a,
                           TransposePositionsIterator const& b)
    {
        return a.px == b.px;
    }
    friend bool operator!=(TransposePositionsIterator const& a,
                           TransposePositionsIterator const& b)
    {
        return a.px != b.px;
    }
    friend bool operator<(TransposePositionsIterator const& a,
                          TransposePositionsIterator const& b)
    {
        return a.px < b.px;
    }
    friend bool operator<=(TransposePositionsIterator const& a,
                           TransposePositionsIterator const& b)
    {
        return a.px <= b.px;
    }
    friend bool operator>(TransposePositionsIterator const& a,
                          TransposePositionsIterator const& b)
    {
        return a.px > b.px;
    }
    friend bool operator>=(TransposePositionsIterator const& a,
                           TransposePositionsIterator const& b)
    {
        return a.px >= b.px;
    }
};

bool ValidScatterParam(UsdGenParamValue const& param) {
    if(param.name==TfToken("subdivisionLevel"))
        return param.value.IsHolding<int>() && param.value.UncheckedGet<int>()==0;
    if (param.name == TfToken("density")) return IsFloat(param.value);
    return param.name == TfToken("flip") && param.value.IsHolding<bool>();
}

// Threaded validation scans (verdict-identical): every ValidateSurface
// group below reports the same message for any failing element, and the
// groups keep their serial order with the same early exit between groups,
// so chunking within a group only changes how fast the verdict arrives.
// Small arrays stay serial (dispatch costs more than the scan below ~32K
// elements). Plain TBB: validation runs outside any scheduler arena.
template <class T, class Bad>
bool ValidateAnyBad(T const* data, size_t n, Bad bad) {
    int const workers = tbb::this_task_arena::max_concurrency();
    size_t const chunks =
        (workers > 1 && n > 32768) ? std::min({size_t(workers), size_t(8), n})
                                   : 1;
    if (chunks == 1) {
        for (size_t i = 0; i < n; ++i)
            if (bad(data[i])) return true;
        return false;
    }
    std::vector<unsigned char> partial(chunks, 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chunks),
        [&](tbb::blocked_range<size_t> const& range) {
            for (size_t c = range.begin(); c != range.end(); ++c) {
                size_t const i0 = (c * n) / chunks;
                size_t const i1 = ((c + 1) * n) / chunks;
                bool hit = false;
                for (size_t i = i0; i < i1; ++i) {
                    if (bad(data[i])) { hit = true; break; }
                }
                partial[c] = hit ? 1 : 0;
            }
        });
    for (unsigned char f : partial)
        if (f) return true;
    return false;
}

// Face-count validation with the exact corner total (verdict-identical):
// each chunk scans its slice with the serial spelling (count<3 fails,
// overflow-checked accumulation), then the chunk sums combine in chunk
// order with the same overflow check. All terms are non-negative past
// the count<3 filter, so the running total overflows exactly when the
// grand total exceeds the maximum, and any count<3 fails either way.
// Same verdict, same total, any chunking.
bool ValidateCounts(int const* counts, size_t n, size_t* corners) {
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    int const workers = tbb::this_task_arena::max_concurrency();
    size_t const chunks =
        (workers > 1 && n > 32768) ? std::min({size_t(workers), size_t(8), n})
                                   : 1;
    if (chunks == 1) {
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            int const count = counts[i];
            if (count < 3 || size_t(count) > kMax - total) return false;
            total += size_t(count);
        }
        *corners = total;
        return true;
    }
    struct CountChunk { bool bad = false; size_t sum = 0; };
    std::vector<CountChunk> partial(chunks);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chunks),
        [&](tbb::blocked_range<size_t> const& range) {
            for (size_t c = range.begin(); c != range.end(); ++c) {
                size_t const i0 = (c * n) / chunks;
                size_t const i1 = ((c + 1) * n) / chunks;
                size_t acc = 0;
                bool bad = false;
                for (size_t i = i0; i < i1; ++i) {
                    int const count = counts[i];
                    if (count < 3 || size_t(count) > kMax - acc) {
                        bad = true;
                        break;
                    }
                    acc += size_t(count);
                }
                partial[c].bad = bad;
                partial[c].sum = acc;
            }
        });
    size_t total = 0;
    for (auto const& p : partial) {
        if (p.bad) return false;
        if (p.sum > kMax - total) return false;
        total += p.sum;
    }
    *corners = total;
    return true;
}

CudaScatterInputStatus ValidateSurface(UsdGenSurfaceDesc const& surface,
                                       std::string* reason) {
    if (surface.faceVertexCounts.empty()) {
        if (!surface.faceVertexIndices.empty())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has indices but no faces", reason);
        if (!surface.uv.empty() && surface.uv.size() != surface.restPoints.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface UV cardinality differs from rest positions", reason);
        if (ValidateAnyBad(surface.restPoints.cdata(), surface.restPoints.size(),
                           [](GfVec3f const& p) { return !Finite(p); }))
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has non-finite rest position", reason);
        if (ValidateAnyBad(surface.uv.cdata(), surface.uv.size(),
                           [](GfVec2f const& uv) { return !Finite(uv); }))
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has non-finite UV", reason);
        return CudaScatterInputStatus::Ok;
    }
    if (surface.restPoints.empty())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface faces require rest positions", reason);
    if (!surface.uv.empty() && surface.uv.size() != surface.restPoints.size())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface UV cardinality differs from rest positions", reason);
    size_t corners = 0;
    if (!ValidateCounts(surface.faceVertexCounts.cdata(),
                        surface.faceVertexCounts.size(), &corners))
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface has invalid face cardinality", reason);
    if (corners != surface.faceVertexIndices.size())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface face-index cardinality differs from face counts", reason);
    if (ValidateAnyBad(surface.restPoints.cdata(), surface.restPoints.size(),
                       [](GfVec3f const& p) { return !Finite(p); }))
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface has non-finite rest position", reason);
    if (ValidateAnyBad(surface.uv.cdata(), surface.uv.size(),
                       [](GfVec2f const& uv) { return !Finite(uv); }))
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface has non-finite UV", reason);
    size_t const points = surface.restPoints.size();
    if (ValidateAnyBad(surface.faceVertexIndices.cdata(),
                       surface.faceVertexIndices.size(),
                       [points](int index) {
                           return index < 0 || size_t(index) >= points;
                       }))
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface has out-of-range face index", reason);
    // The subset loop stays serial: range-vs-duplicate precedence is
    // element-order-dependent (the first failing element names the
    // message), and subsets are small (usually empty).
    std::set<int> subset;
    for (int face : surface.subsetFaces)
        if (face < 0 || size_t(face) >= surface.faceVertexCounts.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has out-of-range subset face", reason);
        else if (!subset.insert(face).second)
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has duplicate subset face", reason);
    return CudaScatterInputStatus::Ok;
}

} // namespace

CudaScatterInputStatus PrepareCudaScatterInput(
    UsdGenGraphDesc const& desc, SdfPath const& scatterPath,
    std::shared_ptr<const gpu::ScatterGrowRoots>* out, std::string* reason) {
    if (reason) reason->clear();
    if (!out || scatterPath.IsEmpty())
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input needs an output owner and node path", reason);
    auto nodeIt = std::find_if(desc.nodes.begin(), desc.nodes.end(),
        [&](UsdGenNodeDesc const& node) { return node.path == scatterPath; });
    if (nodeIt == desc.nodes.end() || nodeIt->type != TfToken("UsdGenScatter"))
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input path does not resolve to a Scatter node", reason);
    UsdGenNodeDesc const& node = *nodeIt;
    // 02 §6.3: a disabled generator publishes an EMPTY curve set, so the
    // description publishes no curves. Every operator downstream of the empty
    // root set is a no-op, which is exactly the CPU lane's behaviour. Any
    // disabled generator in the description empties this root set: Grow and
    // CurveSource are fused into the same native slice and have no separate
    // producer to empty.
    {
        usdGenRegisterM1Operators();
        bool generatorDisabled = false;
        for (UsdGenNodeDesc const& candidate : desc.nodes) {
            if (candidate.enabled) continue;
            std::unique_ptr<UsdGenOp> probe =
                UsdGenOpRegistry::Get().Create(candidate.type);
            generatorDisabled = generatorDisabled || (probe && probe->IsGenerator());
        }
        if (generatorDisabled) {
            *out = std::make_shared<gpu::ScatterGrowRoots>();
            return CudaScatterInputStatus::Ok;
        }
    }
    if (!node.mode.IsEmpty())
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter has no usdGen:mode property; it is always random", reason);
    if (!node.inputs.empty() || !node.references.empty() || !node.curves.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() || !node.expressionBindings.empty() ||
        !node.ramps.empty())
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not support geometry/reference/map/expression/ramp controls", reason);
    if (node.surfaces.size() != 1)
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input requires exactly one surface path", reason);
    std::set<TfToken> seen;
    for (UsdGenParamValue const& param : node.params) {
        if (param.animated || !seen.insert(param.name).second ||
            !AllowedScatterParam(param.name) || !ValidScatterParam(param))
            return Fail(CudaScatterInputStatus::Unsupported,
                        "Scatter input has unsupported, duplicate, or animated parameter '" +
                        param.name.GetString() + "'", reason);
    }
    auto surfaceIt = std::find_if(desc.surfaces.begin(), desc.surfaces.end(),
        [&](UsdGenSurfaceDesc const& surface) { return surface.path == node.surfaces.front(); });
    if (surfaceIt == desc.surfaces.end())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter input cannot resolve its surface path", reason);
    CudaScatterInputStatus validated = ValidateSurface(*surfaceIt, reason);
    if (validated != CudaScatterInputStatus::Ok) return validated;
    if (surfaceIt->restFromCurrentPoints)
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input requires authored rest points, not current points", reason);

    UsdGenParamView params{&desc, &node};
    if (params.GetBool(TfToken("flip"), false))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not yet support flip frame handedness", reason);
    std::unique_ptr<UsdGenOp> op = UsdGenOpRegistry::Get().Create(TfToken("UsdGenScatter"));
    if (!op || op->GeometryInputArity() != 0)
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input cannot resolve the authoritative random Scatter kernel", reason);
    UsdGenDiagnostics diagnostics;
    if (!op->Bind(params, &diagnostics))
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    diagnostics.errors.empty() ? "Scatter Bind failed" : diagnostics.errors.front(), reason);
    std::unique_ptr<UsdGenCapture> capture = op->CreateCapture();
    if (!capture)
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    "Scatter kernel did not create capture storage", reason);
    UsdGenCurveBuffer emptyUpstream;
    UsdGenCaptureContext context;
    context.desc = &desc;
    context.params = &params;
    context.surface = static_cast<UsdGenSurfaceId>(surfaceIt - desc.surfaces.begin());
    context.seed = static_cast<uint32_t>(node.seed);
    context.diag = &diagnostics;
    if (!op->Capture(context, emptyUpstream, capture.get(), &diagnostics))
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    diagnostics.errors.empty() ? "Scatter Capture failed" : diagnostics.errors.front(), reason);
    UsdGenCurveBuffer const& roots = capture->Buffer();
    size_t const n = roots.totalCurves;
    if (roots.totalCvs != n || roots.px.size() != n || roots.py.size() != n ||
        roots.pz.size() != n || roots.curveId.size() != n || roots.rootPrim.size() != n ||
        roots.rootUV.size() != n || roots.rootT.size() != n || roots.rootB.size() != n ||
        roots.rootN.size() != n)
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    "Scatter Capture produced an invalid root topology", reason);
    auto prepared = std::make_shared<gpu::ScatterGrowRoots>();
    // Layout-identical planes copy whole instead of element-wise: GfVec2f
    // (GfVec3f) is 2 (3) contiguous floats, the same bytes as float2
    // (float3). Only positions transposes from the SoA point planes.
    static_assert(sizeof(float2) == sizeof(GfVec2f), "float2/GfVec2f layout");
    static_assert(sizeof(float3) == sizeof(GfVec3f), "float3/GfVec3f layout");
    // reserve+assign instead of resize+memcpy/loop: resize value-inits
    // (zeroes) every element through the non-scalar fill loop and the
    // copy then overwrites them all. assign copies straight into
    // uninitialized storage with identical bytes.
    // Every plane is an independent function of the capture buffer into a
    // disjoint destination vector, so the seven copies run over workers
    // for big inputs (plain TBB: this runs outside any scheduler arena)
    // and serially below the threshold. Same bytes, any order.
    float const* uv = nullptr;
    float const* rt = nullptr;
    float const* rb = nullptr;
    float const* rn = nullptr;
    if (n) {
        uv = reinterpret_cast<float const*>(roots.rootUV.cdata());
        rt = reinterpret_cast<float const*>(roots.rootT.cdata());
        rb = reinterpret_cast<float const*>(roots.rootB.cdata());
        rn = reinterpret_cast<float const*>(roots.rootN.cdata());
    }
    auto* prep = prepared.get();
    auto copyPlane = [&](size_t i) {
        switch (i) {
        case 0:
            prep->stableIds.assign(roots.curveId.cbegin(), roots.curveId.cend());
            break;
        case 1:
            prep->rootPrim.assign(roots.rootPrim.cbegin(), roots.rootPrim.cend());
            break;
        case 2:
            prep->rootUV.reserve(n);
            if (n)
                prep->rootUV.assign(FloatRunIterator<float2, 2>(uv),
                                    FloatRunIterator<float2, 2>(uv + 2 * n));
            break;
        case 3:
            prep->rootT.reserve(n);
            if (n)
                prep->rootT.assign(FloatRunIterator<float3, 3>(rt),
                                   FloatRunIterator<float3, 3>(rt + 3 * n));
            break;
        case 4:
            prep->rootB.reserve(n);
            if (n)
                prep->rootB.assign(FloatRunIterator<float3, 3>(rb),
                                   FloatRunIterator<float3, 3>(rb + 3 * n));
            break;
        case 5:
            prep->rootN.reserve(n);
            if (n)
                prep->rootN.assign(FloatRunIterator<float3, 3>(rn),
                                   FloatRunIterator<float3, 3>(rn + 3 * n));
            break;
        default:
            prep->positions.reserve(n);
            if (n)
                prep->positions.assign(
                    TransposePositionsIterator(roots.px.cdata(), roots.py.cdata(),
                                               roots.pz.cdata()),
                    TransposePositionsIterator(roots.px.cdata() + n,
                                               roots.py.cdata() + n,
                                               roots.pz.cdata() + n));
            break;
        }
    };
    int const convertWorkers = tbb::this_task_arena::max_concurrency();
    if (n > 32768 && convertWorkers > 1) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, 7),
            [&](tbb::blocked_range<size_t> const& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                    copyPlane(i);
            });
    } else {
        for (size_t i = 0; i < 7; ++i)
            copyPlane(i);
    }
    *out = std::move(prepared);
    return CudaScatterInputStatus::Ok;
}

} // namespace usdGen
