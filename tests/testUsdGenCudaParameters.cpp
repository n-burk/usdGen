#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/half.h"
#include "cudaParameters.h"
#include "gpu/curveSource.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
using namespace usdGen;
int main() {
    int failures = 0;
    auto check = [&](bool condition, char const* message) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    UsdGenGraphDesc graph;
    UsdGenExpressionDesc expression;
    expression.path = SdfPath("/Expr"); expression.source = "$value * 2";
    expression.outputs.push_back({TfToken("result"), TfToken("float"), {expr::ScalarType::Float32,1,1,1,1,false}});
    graph.expressions.push_back(expression);
    UsdGenNodeDesc node;
    UsdGenExpressionBinding binding;
    binding.expression = expression.path; binding.destination = TfToken("width");
    binding.destinationShape.scalar = expr::ScalarType::Float32;
    binding.destinationShape.components = 1; binding.literal = VtValue(0.5f);
    node.expressionBindings.push_back(binding);
    CudaParameterPlan plan; std::vector<std::string> diagnostics;
    auto status = CudaParameterPlan::Compile(graph, node, &plan, &diagnostics);
    if (status != CudaParameterStatus::Ok || plan.Bindings().size() != 1 || !plan.Fields().empty() || plan.Find(TfToken("width"))) {
        std::fprintf(stderr, "parameter plan compile failed (%d)\n", int(status));
        for (auto const& message : diagnostics) std::fprintf(stderr, "  %s\n", message.c_str());
        return 1;
    }
    // Compilation remains CPU-only and may contain all three consumer rates.
    for (auto domain : {expr::Domain::Groom, expr::Domain::Primitive, expr::Domain::Point}) {
        UsdGenExpressionBinding b = binding;
        b.destination = TfToken(domain == expr::Domain::Groom ? "enabled" : domain == expr::Domain::Primitive ? "seed" : "widthPoint");
        b.domain = domain;
        node.expressionBindings.push_back(b);
    }
    CudaParameterPlan mixed;
    if (CudaParameterPlan::Compile(graph, node, &mixed, &diagnostics) != CudaParameterStatus::Ok ||
        mixed.Bindings().size() != 4) {
        std::fprintf(stderr, "mixed-domain parameter plan compile failed\n"); return 1;
    }
    auto malformed = node;
    malformed.expressionBindings.back().destinationShape.isArray = true;
    if (CudaParameterPlan::Compile(graph, malformed, &mixed, &diagnostics) != CudaParameterStatus::UnsupportedType) {
        std::fprintf(stderr, "malformed parameter shape was accepted\n"); return 1;
    }
    auto wrongLiteral = node;
    wrongLiteral.expressionBindings[0].literal = VtValue(double(.5));
    check(CudaParameterPlan::Compile(graph, wrongLiteral, &mixed, &diagnostics) == CudaParameterStatus::UnsupportedType,
          "reject non-native float literal");
    auto hugeInteger = binding;
    hugeInteger.destination = TfToken("largeInt");
    hugeInteger.nativeType = TfToken("int64");
    hugeInteger.destinationShape.scalar = expr::ScalarType::Int64;
    hugeInteger.literal = VtValue(int64_t(1) << 54);
    UsdGenExpressionDesc intExpression;
    intExpression.path = SdfPath("/IntExpr"); intExpression.source = "$value";
    intExpression.outputs.push_back({TfToken("result"), TfToken("int64"),
        {expr::ScalarType::Int64, 1, 1, 1, 1, false}});
    graph.expressions.push_back(intExpression);
    hugeInteger.expression = intExpression.path;
    UsdGenNodeDesc intNode; intNode.expressionBindings.push_back(hugeInteger);
    check(CudaParameterPlan::Compile(graph, intNode, &mixed, &diagnostics) == CudaParameterStatus::UnsupportedType,
          "reject int64 literal beyond exact double range");
    UsdGenNodeDesc duplicateNode;
    auto duplicateA = binding; duplicateA.destination = TfToken("width");
    auto duplicateB = binding; duplicateB.destination = TfToken("usdGen:width");
    duplicateNode.expressionBindings = {duplicateA, duplicateB};
    check(CudaParameterPlan::Compile(graph, duplicateNode, &mixed, &diagnostics) == CudaParameterStatus::InvalidArgument,
          "reject canonical duplicate destinations");
    auto badOutputGraph = graph;
    badOutputGraph.expressions.front().outputs.front().shape.elementCount = 2;
    check(CudaParameterPlan::Compile(badOutputGraph, node, &mixed, &diagnostics) == CudaParameterStatus::InvalidArgument,
          "reject output element-count mismatch");

    auto addExpression = [&](char const* path, char const* source) {
        UsdGenExpressionDesc e; e.path = SdfPath(path); e.source = source;
        e.outputs.push_back({TfToken("result"), TfToken("float"),
                             {expr::ScalarType::Float32, 1, 1, 1, 1, false}});
        graph.expressions.push_back(std::move(e));
    };
    addExpression("/Frame", "$frame + $value");
    addExpression("/Prim", "$primIndex + $value");
    addExpression("/Point", "$P[0] + $value");
    addExpression("/Bool", "$frame >= 2");
    graph.expressions.back().outputs[0].nativeType = TfToken("bool");
    graph.expressions.back().outputs[0].shape.scalar = expr::ScalarType::Bool;
    UsdGenNodeDesc runtimeNode;
    auto addBinding = [&](char const* expression, char const* destination,
                          expr::Domain domain, expr::ScalarType scalar, float literal) {
        UsdGenExpressionBinding b; b.expression = SdfPath(expression);
        b.destination = TfToken(destination); b.domain = domain;
        b.destinationShape = {scalar, 1, 1, 1, 1, false}; b.literal = VtValue(literal);
        runtimeNode.expressionBindings.push_back(std::move(b));
    };
    addBinding("/Frame", "frameValue", expr::Domain::Groom, expr::ScalarType::Float32, .5f);
    addBinding("/Prim", "primValue", expr::Domain::Primitive, expr::ScalarType::Float32, 1.f);
    addBinding("/Point", "pointValue", expr::Domain::Point, expr::ScalarType::Float32, 2.f);
    addBinding("/Bool", "frameBool", expr::Domain::Groom, expr::ScalarType::Bool, 0.f);
    runtimeNode.expressionBindings.back().nativeType = TfToken("bool");
    runtimeNode.expressionBindings.back().literal = VtValue(false);
    CudaParameterPlan runtimePlan;
    if (CudaParameterPlan::Compile(graph, runtimeNode, &runtimePlan, &diagnostics) != CudaParameterStatus::Ok) {
        std::fprintf(stderr, "runtime parameter plan compile failed\n");
        for (auto const& message : diagnostics) std::fprintf(stderr, "  %s\n", message.c_str());
        return 1;
    }
    cudaStream_t stream = nullptr;
    auto streamStatus = cudaStreamCreate(&stream);
    if (streamStatus != cudaSuccess) {
        std::fprintf(stderr, "CUDA stream creation failed: %s\n", cudaGetErrorString(streamStatus));
        return 2;
    }
    std::vector<int32_t> counts{2, 3};
    std::vector<float3> points{{1,0,0},{2,0,0},{10,0,0},{11,0,0},{12,0,0}};
    std::vector<float> widths(points.size(), 1.f);
    std::vector<uint64_t> ids{4, 9};
    gpu::CurveSourceInput sourceInput;
    sourceInput.curveVertexCounts = {counts.data(), counts.size()};
    sourceInput.points = {points.data(), points.size()};
    sourceInput.widths = {widths.data(), widths.size()};
    sourceInput.stableIds = {ids.data(), ids.size()};
    gpu::CudaCurveSource curveSource;
    if (curveSource.Set(sourceInput, stream) != gpu::CurveSourceStatus::Ok ||
        curveSource.Finish(stream) != gpu::CurveSourceStatus::Ok) {
        std::fprintf(stderr, "curve source setup failed\n"); return 2;
    }
    for (int frame = 1; frame <= 2; ++frame) {
        expr::Context controls; controls.domain = expr::Domain::Point;
        controls.frame = frame;
        if (runtimePlan.Evaluate(curveSource.view(), {}, controls, stream, &diagnostics) != CudaParameterStatus::Ok) {
            std::fprintf(stderr, "runtime parameter evaluation failed at frame %d\n", frame); return 1;
        }
        auto const* frameField = runtimePlan.Find(TfToken("frameValue"));
        auto const* primField = runtimePlan.Find(TfToken("primValue"));
        auto const* pointField = runtimePlan.Find(TfToken("pointValue"));
        auto const* boolField = runtimePlan.Find(TfToken("frameBool"));
        check(frameField && primField && pointField && boolField, "publish all mixed-domain fields");
        if (!frameField || !primField || !pointField || !boolField) continue;
        float frameValue = 0, primValue[2]{}, pointValue[5]{}; unsigned char boolValue = 0;
        check(cudaMemcpy(&frameValue, frameField->data, sizeof(frameValue), cudaMemcpyDeviceToHost) == cudaSuccess, "read groom result");
        check(cudaMemcpy(primValue, primField->data, sizeof(primValue), cudaMemcpyDeviceToHost) == cudaSuccess, "read primitive results");
        check(cudaMemcpy(pointValue, pointField->data, sizeof(pointValue), cudaMemcpyDeviceToHost) == cudaSuccess, "read point results");
        check(cudaMemcpy(&boolValue, boolField->data, sizeof(boolValue), cudaMemcpyDeviceToHost) == cudaSuccess, "read boolean result");
        check(std::fabs(frameValue - (frame + .5f)) < 1e-5f, "groom frame reevaluation");
        check(std::fabs(primValue[0]-1.f)<1e-5f && std::fabs(primValue[1]-2.f)<1e-5f, "primitive values/count");
        for (unsigned i=0;i<5;++i) check(std::fabs(pointValue[i]-(points[i].x+2.f))<1e-5f, "point values/count");
        check(boolValue == (frame >= 2), "typed bool output");
    }

    auto addTypedExpression = [&](char const* path, char const* source,
                                  char const* nativeType, expr::ValueShape shape) {
        UsdGenExpressionDesc e; e.path = SdfPath(path); e.source = source;
        e.outputs.push_back({TfToken("result"), TfToken(nativeType), shape});
        graph.expressions.push_back(std::move(e));
    };
    addTypedExpression("/Half", "$value * 2", "half",
        {expr::ScalarType::Float16, 1, 1, 1, 1, false});
    addTypedExpression("/Double3", "$value * 2", "double3",
        {expr::ScalarType::Float64, 1, 3, 1, 1, false});
    addTypedExpression("/Float2", "$value * 2", "float2",
        {expr::ScalarType::Float32, 1, 2, 1, 1, false});
    UsdGenNodeDesc typedNode;
    UsdGenExpressionBinding halfBinding;
    halfBinding.expression = SdfPath("/Half"); halfBinding.destination = TfToken("halfValue");
    halfBinding.nativeType = TfToken("half");
    halfBinding.destinationShape = {expr::ScalarType::Float16, 1, 1, 1, 1, false};
    halfBinding.literal = VtValue(GfHalf(.75f));
    typedNode.expressionBindings.push_back(halfBinding);
    UsdGenExpressionBinding doubleBinding;
    doubleBinding.expression = SdfPath("/Double3"); doubleBinding.destination = TfToken("doubleValue");
    doubleBinding.nativeType = TfToken("double3");
    doubleBinding.destinationShape = {expr::ScalarType::Float64, 1, 3, 1, 1, false};
    doubleBinding.literal = VtValue(GfVec3d(1.25, -2.5, 4.0));
    typedNode.expressionBindings.push_back(doubleBinding);
    UsdGenExpressionBinding floatBinding;
    floatBinding.expression = SdfPath("/Float2"); floatBinding.destination = TfToken("floatValue");
    floatBinding.nativeType = TfToken("float2");
    floatBinding.destinationShape = {expr::ScalarType::Float32, 1, 2, 1, 1, false};
    floatBinding.literal = VtValue(GfVec2f(2.f, -3.f));
    typedNode.expressionBindings.push_back(floatBinding);
    CudaParameterPlan typedPlan;
    check(CudaParameterPlan::Compile(graph, typedNode, &typedPlan, &diagnostics) == CudaParameterStatus::Ok,
          "compile half and vector literal plan");
    if (typedPlan.Bindings().size() == 3 &&
        typedPlan.Evaluate(curveSource.view(), {}, {}, stream, &diagnostics) == CudaParameterStatus::Ok) {
        auto const* halfField = typedPlan.Find(TfToken("halfValue"));
        auto const* doubleField = typedPlan.Find(TfToken("doubleValue"));
        auto const* floatField = typedPlan.Find(TfToken("floatValue"));
        check(halfField && doubleField && floatField, "publish typed literal fields");
        if (halfField && doubleField && floatField) {
            __half halfResult{}; double doubleResult[3]{}; float floatResult[2]{};
            check(cudaMemcpy(&halfResult, halfField->data, sizeof(halfResult), cudaMemcpyDeviceToHost) == cudaSuccess,
                  "read half result");
            check(cudaMemcpy(doubleResult, doubleField->data, sizeof(doubleResult), cudaMemcpyDeviceToHost) == cudaSuccess,
                  "read double3 result");
            check(cudaMemcpy(floatResult, floatField->data, sizeof(floatResult), cudaMemcpyDeviceToHost) == cudaSuccess,
                  "read float2 result");
            check(std::fabs(__half2float(halfResult) - 1.5f) < 1e-3f, "half literal precision");
            check(std::fabs(doubleResult[0] - 2.5) < 1e-12 &&
                  std::fabs(doubleResult[1] + 5.0) < 1e-12 &&
                  std::fabs(doubleResult[2] - 8.0) < 1e-12, "double3 literal precision");
            check(std::fabs(floatResult[0] - 4.f) < 1e-5f &&
                  std::fabs(floatResult[1] + 6.f) < 1e-5f, "float2 literal precision");
        }
    } else {
        check(false, "evaluate half and vector literal plan");
    }
    cudaStreamDestroy(stream);
    return failures;
}
