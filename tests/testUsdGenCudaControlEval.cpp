#include "usdGen/cudaExecution.h"
#include "usdGen/gpu/generation.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <cuda_runtime_api.h>

using namespace usdGen;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

namespace {

UsdGenGraphDesc Base() {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/Controls");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Controls/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2, 3};
    curves.curveId = {42, 7};
    curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                     {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
    curves.rest = curves.points;
    curves.skinPrim = {0, 0};
    curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
    desc.curveSets.push_back(curves);
    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/Controls/Scalp");
    surface.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    surface.points = surface.restPoints;
    surface.faceVertexCounts = {3,3,3};
    surface.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    desc.surfaces.push_back(surface);
    UsdGenNodeDesc source;
    source.path = SdfPath("/Controls/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {surface.path};
    desc.nodes.push_back(source);
    return desc;
}

void AddExpression(UsdGenGraphDesc* desc, UsdGenNodeDesc* node,
                   char const* path, char const* source, char const* destination,
                   expr::Domain domain, float literal) {
    UsdGenExpressionDesc expression;
    expression.path = SdfPath(path);
    expression.source = source;
    expression.outputs.push_back({TfToken("result"), TfToken("float"),
                                  {expr::ScalarType::Float32, 1, 1, 1, 1, false}});
    desc->expressions.push_back(expression);
    UsdGenExpressionBinding binding;
    binding.expression = expression.path;
    binding.destination = TfToken(destination);
    binding.domain = domain;
    binding.nativeType = TfToken("float");
    binding.destinationShape = {expr::ScalarType::Float32, 1, 1, 1, 1, false};
    binding.literal = VtValue(literal);
    node->expressionBindings.push_back(std::move(binding));
}

bool ReadWidths(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
                std::vector<float>* widths) {
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    bool ok = false;
    {
        auto lease = gpu::AcquireGeometry(generation, stream);
        if (lease) {
            auto const geometry = lease.Geometry();
            widths->resize(geometry.widths.size);
            ok = (widths->empty() || cudaMemcpyAsync(widths->data(),
                    geometry.widths.data, widths->size() * sizeof(float),
                    cudaMemcpyDeviceToHost, stream) == cudaSuccess) &&
                cudaStreamSynchronize(stream) == cudaSuccess;
        }
    }
    ok = cudaStreamSynchronize(stream) == cudaSuccess && ok;
    cudaStreamDestroy(stream);
    return ok;
}

bool WidthsAre(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
               std::vector<float> const& expected) {
    std::vector<float> widths;
    if (!ReadWidths(generation, &widths) || widths.size() != expected.size())
        return false;
    for (size_t i = 0; i != widths.size(); ++i)
        if (std::fabs(widths[i] - expected[i]) > 1e-5f) {
            std::fprintf(stderr, "width %zu: %g != %g\n", i, widths[i], expected[i]);
            return false;
        }
    return true;
}

bool CountsAre(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
               unsigned curves, unsigned points) {
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    bool ok = false;
    {
        auto lease = gpu::AcquireGeometry(generation, stream);
        if (lease) {
            auto const geometry = lease.Geometry();
            ok = geometry.curveCount == curves && geometry.pointCount == points;
            if (!ok) std::fprintf(stderr, "counts %u/%u\n",
                geometry.curveCount, geometry.pointCount);
        }
    }
    ok = cudaStreamSynchronize(stream) == cudaSuccess && ok;
    cudaStreamDestroy(stream);
    return ok;
}

bool CurveIdsAre(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
                 std::vector<int> const& expected) {    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    bool ok = false;
    {
        auto lease = gpu::AcquireGeometry(generation, stream);
        if (lease) {
            auto const geometry = lease.Geometry();
            std::vector<uint64_t> ids(geometry.stableIds.size);
            ok = (ids.empty() || cudaMemcpyAsync(ids.data(), geometry.stableIds.data,
                    ids.size() * sizeof(uint64_t), cudaMemcpyDeviceToHost,
                    stream) == cudaSuccess) &&
                cudaStreamSynchronize(stream) == cudaSuccess;
            ok = ok && geometry.curveCount == expected.size() && ids.size() == expected.size();
            for (size_t i = 0; ok && i != ids.size(); ++i)
                ok = static_cast<int>(ids[i]) == expected[i];
            if (!ok) std::fprintf(stderr, "curveCount=%u ids=%zu\n",
                geometry.curveCount, ids.size());
        }
    }
    ok = cudaStreamSynchronize(stream) == cudaSuccess && ok;
    cudaStreamDestroy(stream);
    return ok;
}

UsdGenNodeDesc Width(char const* path, SdfPath input, float factor) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenWidth");
    node.inputs = {input};
    node.params = {{TfToken("width"), VtValue(factor), false}};
    return node;
}

} // namespace

// Consuming-input runtime evaluation proofs: numeric controls across
// groom/primitive/point domains with typed broadcasting, bool controls,
// and primitive-to-point inheritance. Every cell asserts device values,
// not just successful completion.
int main() {
    UsdGenDiagnostics diagnostics;

    // Primitive-domain float: per-curve widths from rootUV ($u = .2/.3).
    // Output curves are stably id-ordered (7 before 42), so the 3-point
    // curve carries .3 and the 2-point curve carries .2.
    {
        auto desc = Base();
        auto width = Width("/Controls/W", SdfPath("/Controls/Source"), 1.f);
        AddExpression(&desc, &width, "/Controls/ExprPrim", "$value * $u",
                      "width", expr::Domain::Primitive, 1.f);
        desc.nodes.push_back(width);
        desc.terminal = width.path;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto generation = ExecuteCudaGraph(*plan, *workspace, 1, 1, &diagnostics);
        if (!generation || diagnostics.HasErrors())
            for (auto const& error : diagnostics.errors)
                std::fprintf(stderr, "primcell: %s\n", error.c_str());
        CHECK(generation && !diagnostics.HasErrors() &&
              WidthsAre(generation, {.3f, .3f, .3f, .2f, .2f}));
    }

    // Point-domain float: per-point widths from hairT ($t = 0/1, 0/.5/1),
    // in stable id order.
    {
        auto desc = Base();
        auto width = Width("/Controls/W", SdfPath("/Controls/Source"), 1.f);
        AddExpression(&desc, &width, "/Controls/ExprPoint", "$value * $t",
                      "width", expr::Domain::Point, 1.f);
        desc.nodes.push_back(width);
        desc.terminal = width.path;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto generation = ExecuteCudaGraph(*plan, *workspace, 1, 2, &diagnostics);
        CHECK(generation && !diagnostics.HasErrors() &&
              WidthsAre(generation, {0.f, .5f, 1.f, 0.f, 1.f}));
    }

    // Point-domain inheritance: primitive rootUV consumed per point.
    {
        auto desc = Base();
        auto width = Width("/Controls/W", SdfPath("/Controls/Source"), 1.f);
        AddExpression(&desc, &width, "/Controls/ExprInherit", "$value * $u",
                      "width", expr::Domain::Point, 1.f);
        desc.nodes.push_back(width);
        desc.terminal = width.path;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto generation = ExecuteCudaGraph(*plan, *workspace, 1, 3, &diagnostics);
        CHECK(generation && !diagnostics.HasErrors() &&
              WidthsAre(generation, {.3f, .3f, .3f, .2f, .2f}));
    }

    // Disabled controls are an exact no-op: a muted Width aliases its input.
    {
        auto desc = Base();
        auto first = Width("/Controls/W1", SdfPath("/Controls/Source"), 2.f);
        auto muted = Width("/Controls/W2", first.path, 9.f);
        muted.enabled = false;
        desc.nodes.push_back(first);
        desc.nodes.push_back(muted);
        desc.terminal = muted.path;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto generation = ExecuteCudaGraph(*plan, *workspace, 1, 4, &diagnostics);
        auto referenceDesc = Base();
        auto referenceWidth = Width("/Controls/W1", SdfPath("/Controls/Source"), 2.f);
        referenceDesc.nodes.push_back(referenceWidth);
        referenceDesc.terminal = referenceWidth.path;
        auto referencePlan = CompileCudaGraph(referenceDesc, &diagnostics);
        CHECK(referencePlan && !diagnostics.HasErrors());
        auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(referenceWorkspace);
        auto reference = ExecuteCudaGraph(*referencePlan, *referenceWorkspace,
                                          1, 5, &diagnostics);
        CHECK(generation && reference && !diagnostics.HasErrors() &&
              CountsAre(generation, 2, 5) && CountsAre(reference, 2, 5) &&
              WidthsAre(generation, {2.f, 2.f, 2.f, 2.f, 2.f}) &&
              WidthsAre(reference, {2.f, 2.f, 2.f, 2.f, 2.f}));
    }

    // Primitive-domain Length cull: thresholds {.4, .6} keep only the second
    // curve (lengths .2 and 1.0), proving per-curve control evaluation.
    {
        auto desc = Base();
        UsdGenNodeDesc length;
        length.path = SdfPath("/Controls/L");
        length.type = TfToken("UsdGenLength");
        length.inputs = {SdfPath("/Controls/Source")};
        length.params = {{TfToken("length:mode"), VtValue(TfToken("cull")), false}};
        AddExpression(&desc, &length, "/Controls/ExprCull", "$value * $u",
                      "cullThreshold", expr::Domain::Primitive, 2.f);
        desc.nodes.push_back(length);
        desc.terminal = length.path;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto generation = ExecuteCudaGraph(*plan, *workspace, 1, 6, &diagnostics);
        CHECK(generation && !diagnostics.HasErrors() &&
              CurveIdsAre(generation, {7}));
    }
    return 0;
}
