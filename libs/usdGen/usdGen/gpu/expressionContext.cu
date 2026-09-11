#include "expressionContext.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>

namespace usdGen::gpu {
namespace {
using expr::Domain;
using expr::Variable;

constexpr int kBadGeometry = 1;
constexpr int kBadChannel = 2;

__device__ void Fail(int* error, int code) { atomicCAS(error, 0, code); }
__device__ int Error(int const* error) {
    return atomicAdd(const_cast<int*>(error), 0);
}
__device__ bool Finite(float3 const& p) {
    return isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}
__device__ bool Finite(float2 const& p) { return isfinite(p.x) && isfinite(p.y); }

__global__ void Validate(DeviceCurveGeometryView g,
                         ExpressionGeometryChannels channels, int* error) {
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        if (!g.curveOffsets.data ||
            g.curveOffsets.size != g.curveCount + 1 ||
            g.curveOffsets.data[0] != 0 ||
            g.curveOffsets.data[g.curveCount] != g.pointCount)
            Fail(error, kBadGeometry);
    }
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < g.curveCount) {
        uint32_t a = g.curveOffsets.data[i], b = g.curveOffsets.data[i + 1];
        if (a >= b || b > g.pointCount) Fail(error, kBadGeometry);
        if (!g.stableIds.data || i >= g.stableIds.size) Fail(error, kBadGeometry);
    }
    if (i < g.pointCount) {
        if (!g.points.data || i >= g.points.size || !Finite(g.points.data[i]))
            Fail(error, kBadGeometry);
        if (g.restPoints.data && (i >= g.restPoints.size || !Finite(g.restPoints.data[i])))
            Fail(error, kBadGeometry);
        if (g.widths.data && (i >= g.widths.size || !isfinite(g.widths.data[i])))
            Fail(error, kBadGeometry);
        if (channels.hairT.data && (i >= channels.hairT.size ||
                                    !isfinite(channels.hairT.data[i]) ||
                                    channels.hairT.data[i] < 0 || channels.hairT.data[i] > 1))
            Fail(error, kBadChannel);
    }
    if (channels.hairT.data && i < g.curveCount) {
        uint32_t begin = g.curveOffsets.data[i], end = g.curveOffsets.data[i + 1];
        if (begin < end && end <= g.pointCount && end > begin + 1 &&
            (channels.hairT.data[begin] != 0.0f ||
                                channels.hairT.data[end - 1] != 1.0f))
            Fail(error, kBadChannel);
    }
    if (i < g.curveCount && channels.rootUV.data &&
        (i >= channels.rootUV.size || !Finite(channels.rootUV.data[i])))
        Fail(error, kBadChannel);
}

__global__ void BuildOwnersAndArc(DeviceCurveGeometryView g,
                                  uint32_t* owners, double* arc, int* error) {
    size_t c = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (c >= g.curveCount || Error(error) != 0) return;
    uint32_t begin = g.curveOffsets.data[c], end = g.curveOffsets.data[c + 1];
    double total = 0.0;
    for (uint32_t p = begin; p < end; ++p) {
        owners[p] = static_cast<uint32_t>(c);
        if (p > begin) {
            float3 a = g.points.data[p - 1], b = g.points.data[p];
            double dx = double(b.x) - a.x, dy = double(b.y) - a.y, dz = double(b.z) - a.z;
            total += sqrt(dx * dx + dy * dy + dz * dz);
        }
        arc[p] = total;
    }
}

__device__ double T(DeviceCurveGeometryView g, ExpressionGeometryChannels ch,
                    size_t c, size_t p, double const* arc) {
    if (ch.hairT.data) return ch.hairT.data[p];
    uint32_t end = g.curveOffsets.data[c + 1];
    const double total = arc[end - 1];
    return total > 0.0 ? arc[p] / total : 0.0;
}
__device__ void Set3(ExpressionInputs const& out, Variable v, size_t i, float3 p) {
    double* dst = const_cast<double*>(out.fields[(unsigned)v].data);
    dst[i * 3 + 0] = p.x; dst[i * 3 + 1] = p.y; dst[i * 3 + 2] = p.z;
}
__device__ void Set1(ExpressionInputs const& out, Variable v, size_t i, double value) {
    const_cast<double*>(out.fields[(unsigned)v].data)[i] = value;
}

__global__ void BuildFields(DeviceCurveGeometryView g,
                            ExpressionGeometryChannels ch, Domain domain,
                            double const* arc, ExpressionInputs out, int* error) {
    if (Error(error) != 0) return;
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (domain == Domain::Primitive) {
        if (i >= g.curveCount) return;
        uint32_t p = g.curveOffsets.data[i];
        Set3(out, Variable::P, i, g.points.data[p]);
        Set3(out, Variable::RootP, i, g.points.data[p]);
        if (g.restPoints.data) { Set3(out, Variable::PRef, i, g.restPoints.data[p]); Set3(out, Variable::RootPRef, i, g.restPoints.data[p]); }
        if (g.widths.data) Set1(out, Variable::CWidth, i, g.widths.data[p]);
        Set1(out, Variable::T, i, 0.0);
        Set1(out, Variable::PrimIndex, i, double(i));
        Set1(out, Variable::PrimCount, i, double(g.curveCount));
        Set1(out, Variable::CLength, i, arc[g.curveOffsets.data[i + 1] - 1]);
        uint64_t id = g.stableIds.data[i];
        Set1(out, Variable::IdLo, i, double(uint32_t(id)));
        Set1(out, Variable::IdHi, i, double(uint32_t(id >> 32)));
        Set1(out, Variable::Id, i, double(id));
        if (ch.rootUV.data) { Set1(out, Variable::U, i, ch.rootUV.data[i].x); Set1(out, Variable::V, i, ch.rootUV.data[i].y); }
    } else if (domain == Domain::Point) {
        if (i >= g.pointCount) return;
        uint32_t c = out.pointToPrimitive.data[i];
        uint32_t root = g.curveOffsets.data[c];
        Set3(out, Variable::P, i, g.points.data[i]); Set3(out, Variable::RootP, i, g.points.data[root]);
        if (g.restPoints.data) { Set3(out, Variable::PRef, i, g.restPoints.data[i]); Set3(out, Variable::RootPRef, i, g.restPoints.data[root]); }
        if (g.widths.data) Set1(out, Variable::CWidth, i, g.widths.data[i]);
        Set1(out, Variable::T, i, T(g, ch, c, i, arc));
        Set1(out, Variable::PrimIndex, i, double(c));
        Set1(out, Variable::PrimCount, i, double(g.curveCount));
        Set1(out, Variable::CLength, i, arc[g.curveOffsets.data[c + 1] - 1]);
        Set1(out, Variable::PointIndex, i, double(i - root));
        Set1(out, Variable::PointCount, i, double(g.curveOffsets.data[c + 1] - root));
        if (ch.rootUV.data) { Set1(out, Variable::U, i, ch.rootUV.data[c].x); Set1(out, Variable::V, i, ch.rootUV.data[c].y); }
        uint64_t id = g.stableIds.data[c];
        Set1(out, Variable::IdLo, i, double(uint32_t(id)));
        Set1(out, Variable::IdHi, i, double(uint32_t(id >> 32)));
        Set1(out, Variable::Id, i, double(id));
    }
}
} // namespace

CudaExpressionContext::~CudaExpressionContext() {
    if (ready_) { cudaEventSynchronize(ready_); cudaEventDestroy(ready_); }
}

ExpressionInputs const& CudaExpressionContext::Inputs() const {
    static ExpressionInputs const empty = [] {
        ExpressionInputs result;
        result.count = 0;
        result.context.domain = Domain::None;
        result.context.count = 0;
        return result;
    }();
    return usable_ ? inputs_ : empty;
}

ExpressionContextStatus CudaExpressionContext::Build(
    DeviceCurveGeometryView geometry, ExpressionGeometryChannels channels,
    expr::Context context, cudaStream_t stream) {
    if (pending_) return ExpressionContextStatus::InvalidArgument;
    usable_ = false;
    if (context.domain != Domain::Groom && context.domain != Domain::Primitive && context.domain != Domain::Point)
        return ExpressionContextStatus::InvalidArgument;
    auto validView = [](auto view) { return view.data || view.size == 0; };
    if (!validView(geometry.points) || !validView(geometry.restPoints) || !validView(geometry.widths) ||
        !validView(geometry.curveOffsets) || !validView(geometry.stableIds) ||
        !validView(channels.hairT) || !validView(channels.rootUV))
        return ExpressionContextStatus::InvalidArgument;
    if (!std::isfinite(context.frame) || !std::isfinite(context.time))
        return ExpressionContextStatus::InvalidArgument;
    inputs_ = ExpressionInputs{}; inputs_.context = context; inputs_.count = 1;
    if (context.domain == Domain::Groom) {
        if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess) return ExpressionContextStatus::CudaError;
        if (error_.reset(1) != cudaSuccess || cudaMemsetAsync(error_.data(), 0, sizeof(int), stream) != cudaSuccess || cudaEventRecord(ready_, stream) != cudaSuccess) return ExpressionContextStatus::CudaError;
        pending_ = true;
        return ExpressionContextStatus::Ok;
    }
    if ((geometry.curveCount == 0) != (geometry.pointCount == 0) ||
        geometry.curveCount > UINT32_MAX - 1 || geometry.pointCount > UINT32_MAX ||
        !geometry.curveOffsets.data ||
        geometry.curveOffsets.size != geometry.curveCount + 1 ||
        (geometry.pointCount && !geometry.points.data) ||
        geometry.points.size != geometry.pointCount ||
        (geometry.curveCount && !geometry.stableIds.data) ||
        geometry.stableIds.size != geometry.curveCount)
        return ExpressionContextStatus::InvalidGeometry;
    if (geometry.restPoints.data && geometry.restPoints.size != geometry.pointCount)
        return ExpressionContextStatus::InvalidGeometry;
    if (geometry.widths.data && geometry.widths.size != geometry.pointCount)
        return ExpressionContextStatus::InvalidGeometry;
    if (channels.hairT.data && channels.hairT.size != geometry.pointCount)
        return ExpressionContextStatus::InvalidChannel;
    if (channels.rootUV.data && channels.rootUV.size != geometry.curveCount)
        return ExpressionContextStatus::InvalidChannel;
    size_t n = context.domain == Domain::Groom ? 1 :
               context.domain == Domain::Primitive ? geometry.curveCount : geometry.pointCount;
    inputs_.count = n; inputs_.primitiveCount = geometry.curveCount;
    inputs_.context.count = static_cast<uint32_t>(n);
    const unsigned u = static_cast<unsigned>(expr::Variable::CountVariables);
    for (unsigned v = 0; v < u; ++v) {
        bool vec = v == unsigned(Variable::P) || v == unsigned(Variable::PRef) || v == unsigned(Variable::RootP) || v == unsigned(Variable::RootPRef);
        if (vec || v == unsigned(Variable::T) || v == unsigned(Variable::CWidth) || v == unsigned(Variable::PointIndex) || v == unsigned(Variable::PointCount) || v == unsigned(Variable::PrimIndex) || v == unsigned(Variable::PrimCount) || v == unsigned(Variable::CLength) || v == unsigned(Variable::U) || v == unsigned(Variable::V) || v == unsigned(Variable::IdLo) || v == unsigned(Variable::IdHi) || v == unsigned(Variable::Id)) {
            if (v == unsigned(Variable::PRef) || v == unsigned(Variable::RootPRef)) { if (!geometry.restPoints.data) continue; }
            if (v == unsigned(Variable::CWidth) && !geometry.widths.data) continue;
            if ((v == unsigned(Variable::U) || v == unsigned(Variable::V)) && !channels.rootUV.data) continue;
        if (vec && n > std::numeric_limits<size_t>::max() / 3) return ExpressionContextStatus::InvalidArgument;
            if (context.domain == Domain::Primitive && (v == unsigned(Variable::PointIndex) || v == unsigned(Variable::PointCount))) continue;
            if (fields_[v].reset(n * (vec ? 3 : 1)) != cudaSuccess) return ExpressionContextStatus::CudaError;
            inputs_.fields[v] = {fields_[v].data(), n, context.domain, vec ? 3u : 1u};
        }
    }
    if (owners_.reset(geometry.pointCount) != cudaSuccess || arcLength_.reset(geometry.pointCount) != cudaSuccess || error_.reset(1) != cudaSuccess) return ExpressionContextStatus::CudaError;
    inputs_.pointToPrimitive = {owners_.data(), geometry.pointCount};
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess) return ExpressionContextStatus::CudaError;
    if (cudaMemsetAsync(error_.data(), 0, sizeof(int), stream) != cudaSuccess) return ExpressionContextStatus::CudaError;
    const unsigned blocks = std::max(1u, static_cast<unsigned>((std::max(geometry.curveCount, geometry.pointCount) + 127) / 128));
    Validate<<<blocks,128,0,stream>>>(geometry, channels, error_.data());
    if (geometry.curveCount)
        BuildOwnersAndArc<<<(geometry.curveCount + 127) / 128,128,0,stream>>>(geometry, owners_.data(), arcLength_.data(), error_.data());
    BuildFields<<<blocks,128,0,stream>>>(geometry, channels, context.domain, arcLength_.data(), inputs_, error_.data());
    if (cudaGetLastError() != cudaSuccess || cudaEventRecord(ready_, stream) != cudaSuccess) return ExpressionContextStatus::CudaError;
    pending_ = true;
    return ExpressionContextStatus::Ok;
}

ExpressionContextStatus CudaExpressionContext::Wait(cudaStream_t stream) const {
    return ready_ && cudaStreamWaitEvent(stream, ready_, 0) == cudaSuccess ? ExpressionContextStatus::Ok : ExpressionContextStatus::CudaError;
}

ExpressionContextStatus CudaExpressionContext::Finish(cudaStream_t stream) {
    if (!pending_ || !ready_ || cudaEventSynchronize(ready_) != cudaSuccess) return ExpressionContextStatus::CudaError;
    int error = 0;
    if (cudaMemcpyAsync(&error, error_.data(), sizeof(error), cudaMemcpyDeviceToHost, stream) != cudaSuccess || cudaStreamSynchronize(stream) != cudaSuccess) return ExpressionContextStatus::CudaError;
    pending_ = false;
    if (error != 0) return error == kBadChannel ? ExpressionContextStatus::InvalidChannel : ExpressionContextStatus::InvalidGeometry;
    usable_ = true;
    return ExpressionContextStatus::Ok;
}
} // namespace usdGen::gpu
