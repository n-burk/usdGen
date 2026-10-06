#include "usdGen/cudaScatterInput.h"

#include "usdGen/opRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>

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

CudaScatterInputStatus ValidateSurface(UsdGenSurfaceDesc const& surface,
                                       std::string* reason) {
    if (surface.faceVertexCounts.empty()) {
        if (!surface.faceVertexIndices.empty())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has indices but no faces", reason);
        if (!surface.uv.empty() && surface.uv.size() != surface.restPoints.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface UV cardinality differs from rest positions", reason);
        for (GfVec3f const& p : surface.restPoints)
            if (!Finite(p)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                        "Scatter surface has non-finite rest position", reason);
        for (GfVec2f const& uv : surface.uv)
            if (!Finite(uv)) return Fail(CudaScatterInputStatus::InvalidSurface,
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
    for (int count : surface.faceVertexCounts) {
        if (count < 3 || size_t(count) > std::numeric_limits<size_t>::max() - corners)
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has invalid face cardinality", reason);
        corners += size_t(count);
    }
    if (corners != surface.faceVertexIndices.size())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface face-index cardinality differs from face counts", reason);
    for (GfVec3f const& p : surface.restPoints)
        if (!Finite(p)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                    "Scatter surface has non-finite rest position", reason);
    for (GfVec2f const& uv : surface.uv)
        if (!Finite(uv)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                     "Scatter surface has non-finite UV", reason);
    for (int index : surface.faceVertexIndices)
        if (index < 0 || size_t(index) >= surface.restPoints.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has out-of-range face index", reason);
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
    prepared->stableIds.assign(roots.curveId.cbegin(), roots.curveId.cend());
    prepared->rootPrim.assign(roots.rootPrim.cbegin(), roots.rootPrim.cend());
    // reserve+assign instead of resize+memcpy/loop: resize value-inits
    // (zeroes) every element through the non-scalar fill loop and the
    // copy then overwrites them all. assign copies straight into
    // uninitialized storage with identical bytes.
    prepared->rootUV.reserve(n);
    prepared->rootT.reserve(n);
    prepared->rootB.reserve(n);
    prepared->rootN.reserve(n);
    prepared->positions.reserve(n);
    if (n) {
        float const* uv = reinterpret_cast<float const*>(roots.rootUV.cdata());
        float const* rt = reinterpret_cast<float const*>(roots.rootT.cdata());
        float const* rb = reinterpret_cast<float const*>(roots.rootB.cdata());
        float const* rn = reinterpret_cast<float const*>(roots.rootN.cdata());
        prepared->rootUV.assign(FloatRunIterator<float2, 2>(uv),
                               FloatRunIterator<float2, 2>(uv + 2 * n));
        prepared->rootT.assign(FloatRunIterator<float3, 3>(rt),
                              FloatRunIterator<float3, 3>(rt + 3 * n));
        prepared->rootB.assign(FloatRunIterator<float3, 3>(rb),
                              FloatRunIterator<float3, 3>(rb + 3 * n));
        prepared->rootN.assign(FloatRunIterator<float3, 3>(rn),
                              FloatRunIterator<float3, 3>(rn + 3 * n));
        prepared->positions.assign(
            TransposePositionsIterator(roots.px.cdata(), roots.py.cdata(),
                                       roots.pz.cdata()),
            TransposePositionsIterator(roots.px.cdata() + n,
                                       roots.py.cdata() + n,
                                       roots.pz.cdata() + n));
    }
    *out = std::move(prepared);
    return CudaScatterInputStatus::Ok;
}

} // namespace usdGen
