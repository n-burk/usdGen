#include "usdGen/cudaExecution.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace usdGen;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

namespace {

UsdGenCurveSetDesc Curves() {
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Groom/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2};
    curves.points = {{.2f,.2f,0},{.2f,.4f,0}};
    curves.rest = curves.points;
    curves.curveId = {7};
    curves.skinPrim = {0};
    curves.skinPrimUv = {{.2f,.2f}};
    return curves;
}

UsdGenSurfaceDesc Scalp() {
    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/Groom/Scalp");
    surface.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    surface.points = surface.restPoints;
    surface.faceVertexCounts = {3,3,3};
    surface.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    return surface;
}

UsdGenNodeDesc Source() {
    UsdGenNodeDesc node;
    node.path = SdfPath("/Groom/Source");
    node.type = TfToken("UsdGenCurveSource");
    node.curves = {SdfPath("/Groom/Curves")};
    node.surfaces = {SdfPath("/Groom/Scalp")};
    return node;
}

UsdGenNodeDesc Width(char const* path, SdfPath input, float factor = .5f) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenWidth");
    node.inputs = {input};
    node.params = {{TfToken("width"), VtValue(factor), false}};
    return node;
}

UsdGenNodeDesc Length(char const* path, SdfPath input) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenLength");
    node.inputs = {input};
    node.params = {{TfToken("length:mode"), VtValue(TfToken("scale")), false},
                   {TfToken("length:value"), VtValue(.5f), false}};
    return node;
}

UsdGenNodeDesc Noise(char const* path, SdfPath input) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenNoise");
    node.inputs = {input};
    return node;
}

UsdGenNodeDesc Deform(char const* path, SdfPath input, int samples = 5) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenDeform");
    node.inputs = {input};
    node.surfaces = {SdfPath("/Groom/Scalp")};
    node.mode = TfToken("rbf");
    node.readPhase = TfToken("final");
    node.params = {{TfToken("rbfSamples"), VtValue(samples), false}};
    return node;
}

UsdGenNodeDesc Blend(char const* path, SdfPath left, SdfPath right, float weight = .25f) {
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken("UsdGenWidthBlend");
    node.inputs = {left, right};
    node.blend = weight;
    return node;
}

UsdGenGraphDesc Base() {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/Groom");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    desc.surfaces = {Scalp()};
    desc.curveSets = {Curves()};
    return desc;
}

bool Compiles(UsdGenGraphDesc const& desc, UsdGenExecutionPlanShape* shape = nullptr,
              bool* refinable = nullptr, std::string* error = nullptr) {
    UsdGenDiagnostics diagnostics;
    auto plan = CompileCudaGraph(desc, &diagnostics);
    if (!plan || diagnostics.HasErrors()) {
        if (error && !diagnostics.errors.empty()) *error = diagnostics.errors.front();
        return false;
    }
    if (shape || refinable) {
        auto metadata = GetCudaExecutionPlanMetadata(*plan);
        if (!metadata) return false;
        if (shape) *shape = metadata->Shape();
        if (refinable) *refinable = metadata->MemoryEstimate().runtimeRefinementAvailable;
    }
    return true;
}

bool RejectsWith(UsdGenGraphDesc const& desc, char const* text) {
    std::string error;
    if (Compiles(desc, nullptr, nullptr, &error)) {
        std::fprintf(stderr, "expected rejection containing: %s\n", text);
        return false;
    }
    if (error.find(text) == std::string::npos) {
        std::fprintf(stderr, "wrong rejection: %s (want %s)\n", error.c_str(), text);
        return false;
    }
    return true;
}

} // namespace

// Capability/composition matrix probe. Each case locks the layout verdict
// for one composition cell; plan/appendix-C-composition-matrix.md is the
// human-readable matrix these cases enforce.
int main() {
    UsdGenExecutionPlanShape shape = UsdGenExecutionPlanShape::LinearAuthoredChain;
    bool refinable = false;

    // Sources.
    {
        auto desc = Base();
        auto source = Source();
        auto width = Width("/Groom/W", source.path);
        desc.nodes = {source, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag);
    }
    {
        // Reference sources lower to Width DAGs.
        auto desc = Base();
        desc.curveSets.front().role = UsdGenRole::Reference;
        UsdGenNodeDesc source;
        source.path = SdfPath("/Groom/RefSource");
        source.type = TfToken("UsdGenReferenceSource");
        source.references = {SdfPath("/Groom/Curves")};
        auto width = Width("/Groom/W", source.path);
        desc.nodes = {source, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag);
    }
    {
        // Reference-rooted Deform DAGs lower as the third reference shape.
        // Even linear-ordered reference chains lower as DAGs, never legacy.
        auto desc = Base();
        desc.curveSets.front().role = UsdGenRole::Reference;
        UsdGenNodeDesc source;
        source.path = SdfPath("/Groom/RefSource");
        source.type = TfToken("UsdGenReferenceSource");
        source.references = {SdfPath("/Groom/Curves")};
        auto deform = Deform("/Groom/D", source.path);
        auto width = Width("/Groom/W", deform.path);
        desc.nodes = {source, deform, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag && refinable);
    }
    {
        // Scatter requires its single Grow consumer.
        auto desc = Base();
        UsdGenNodeDesc scatter;
        scatter.path = SdfPath("/Groom/Scatter");
        scatter.type = TfToken("UsdGenScatter");
        scatter.surfaces = {SdfPath("/Groom/Scalp")};
        scatter.params = {{TfToken("density"), VtValue(80.f), false}};
        desc.nodes = {scatter};
        desc.terminal = scatter.path;
        CHECK(!Compiles(desc));
    }
    {
        // Scatter -> Grow -> Width is the supported capture route.
        auto desc = Base();
        UsdGenNodeDesc scatter;
        scatter.path = SdfPath("/Groom/Scatter");
        scatter.type = TfToken("UsdGenScatter");
        scatter.surfaces = {SdfPath("/Groom/Scalp")};
        scatter.params = {{TfToken("density"), VtValue(80.f), false}};
        UsdGenNodeDesc grow;
        grow.path = SdfPath("/Groom/Grow");
        grow.type = TfToken("UsdGenGrow");
        grow.inputs = {scatter.path};
        grow.params = {{TfToken("segments"), VtValue(5), false},
                       {TfToken("length"), VtValue(2.f), false}};
        auto width = Width("/Groom/W", grow.path);
        desc.nodes = {scatter, grow, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag);
    }

    // Linear pairs and triples.
    {
        auto desc = Base();
        auto source = Source();
        auto length = Length("/Groom/L", source.path);
        auto width = Width("/Groom/W", length.path);
        desc.nodes = {source, length, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag && refinable);
    }
    {
        // Width prefix + Deform + Width tail is the legacy linear RBF chain.
        auto desc = Base();
        auto source = Source();
        auto prefix = Width("/Groom/P", source.path);
        auto deform = Deform("/Groom/D", prefix.path);
        auto tail = Width("/Groom/W", deform.path);
        desc.nodes = {source, prefix, deform, tail};
        desc.terminal = tail.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::LinearAuthoredChain && refinable);
    }
    {
        // A second Deform along one lineage would apply surface motion twice.
        auto desc = Base();
        auto source = Source();
        auto first = Deform("/Groom/D1", source.path);
        auto second = Deform("/Groom/D2", first.path);
        desc.nodes = {source, first, second};
        desc.terminal = second.path;
        CHECK(RejectsWith(desc, "apply surface motion twice"));
    }
    {
        // Sibling Deforms on independent lineages are legal branches.
        auto desc = Base();
        auto source = Source();
        auto left = Deform("/Groom/DL", source.path);
        auto right = Deform("/Groom/DR", source.path);
        auto wl = Width("/Groom/WL", left.path, 2.f);
        auto wr = Width("/Groom/WR", right.path, 3.f);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        desc.nodes = {source, left, right, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedValueDag && refinable);
    }
    {
        // Noise observes authored rest without a Grow ancestor.
        auto desc = Base();
        auto source = Source();
        auto noise = Noise("/Groom/N", source.path);
        auto width = Width("/Groom/W", noise.path);
        desc.nodes = {source, noise, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc));
    }
    {
        // Length below Width is a value DAG only when a blend fans in;
        // without one it lowers as a unary DAG.
        auto desc = Base();
        auto source = Source();
        auto width = Width("/Groom/W", source.path);
        auto length = Length("/Groom/L", width.path);
        desc.nodes = {source, width, length};
        desc.terminal = length.path;
        CHECK(Compiles(desc, &shape) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag);
    }
    {
        // Scatter -> Grow -> Deform lowers with capture-route refinability.
        auto desc = Base();
        desc.curveSets.clear();
        UsdGenSurfaceDesc scalp;
        scalp.path = SdfPath("/Groom/Scalp");
        scalp.restPoints = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
        scalp.points = scalp.restPoints;
        scalp.faceVertexCounts = {4};
        scalp.faceVertexIndices = {0,1,2,3};
        scalp.uv = {{0,0},{1,0},{1,1},{0,1}};
        desc.surfaces = {scalp};
        UsdGenNodeDesc scatter;
        scatter.path = SdfPath("/Groom/Scatter");
        scatter.type = TfToken("UsdGenScatter");
        scatter.surfaces = {scalp.path};
        scatter.params = {{TfToken("density"), VtValue(80.f), false}};
        UsdGenNodeDesc grow;
        grow.path = SdfPath("/Groom/Grow");
        grow.type = TfToken("UsdGenGrow");
        grow.inputs = {scatter.path};
        grow.params = {{TfToken("segments"), VtValue(5), false},
                       {TfToken("length"), VtValue(2.f), false}};
        auto deform = Deform("/Groom/D", grow.path);
        auto width = Width("/Groom/W", deform.path);
        desc.nodes = {scatter, grow, deform, width};
        desc.terminal = width.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedUnaryDag && refinable);
    }
    {
        // Deform below Length consumes the Length-owned topology snapshot.
        auto desc = Base();
        auto source = Source();
        auto length = Length("/Groom/L", source.path);
        auto deform = Deform("/Groom/D", length.path);
        desc.nodes = {source, length, deform};
        desc.terminal = deform.path;
        CHECK(Compiles(desc));
    }
    {
        // Length-trunk Deform DAGs refine through the RBF recipe.
        auto desc = Base();
        auto source = Source();
        auto length = Length("/Groom/L", source.path);
        auto left = Deform("/Groom/DL", length.path);
        auto right = Deform("/Groom/DR", length.path);
        auto wl = Width("/Groom/WL", left.path, 2.f);
        auto wr = Width("/Groom/WR", right.path, 3.f);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        desc.nodes = {source, length, left, right, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedValueDag && refinable);
    }
    {
        // Linear Grow chains accept only Noise/Length/Width suffixes; a
        // linear Deform suffix stays rejected.
        auto desc = Base();
        auto source = Source();
        UsdGenNodeDesc grow;
        grow.path = SdfPath("/Groom/G");
        grow.type = TfToken("UsdGenGrow");
        grow.inputs = {source.path};
        grow.params = {{TfToken("segments"), VtValue(5), false},
                       {TfToken("length"), VtValue(2.f), false}};
        auto deform = Deform("/Groom/D", grow.path);
        auto width = Width("/Groom/W", deform.path);
        desc.nodes = {source, grow, deform, width};
        desc.terminal = width.path;
        CHECK(RejectsWith(desc, "sequential Noise/Length/Width suffixes"));
    }
    {
        // Branched C3 Grow values feed Deform branches at grown cardinality.
        auto desc = Base();
        auto source = Source();
        UsdGenNodeDesc grow;
        grow.path = SdfPath("/Groom/G");
        grow.type = TfToken("UsdGenGrow");
        grow.inputs = {source.path};
        grow.params = {{TfToken("segments"), VtValue(5), false},
                       {TfToken("length"), VtValue(2.f), false}};
        auto left = Deform("/Groom/DL", grow.path);
        auto right = Deform("/Groom/DR", grow.path);
        auto wl = Width("/Groom/WL", left.path, 2.f);
        auto wr = Width("/Groom/WR", right.path, 3.f);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        desc.nodes = {source, grow, left, right, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedValueDag && refinable);
    }

    // WidthBlend fan-in rules.
    {
        auto desc = Base();
        auto source = Source();
        auto wl = Width("/Groom/WL", source.path, .2f);
        auto wr = Width("/Groom/WR", source.path, .8f);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        desc.nodes = {source, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(Compiles(desc, &shape) &&
              shape == UsdGenExecutionPlanShape::SourceRootedValueDag);
    }
    {
        auto desc = Base();
        auto source = Source();
        auto wl = Width("/Groom/WL", source.path, .2f);
        auto wr = Width("/Groom/WR", source.path, .8f);
        UsdGenNodeDesc blend;
        blend.path = SdfPath("/Groom/B");
        blend.type = TfToken("UsdGenWidthBlend");
        blend.inputs = {wl.path};
        blend.blend = .5f;
        desc.nodes = {source, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(RejectsWith(desc, "exactly two ordered"));
    }
    {
        auto desc = Base();
        auto source = Source();
        auto wl = Width("/Groom/WL", source.path, .2f);
        UsdGenNodeDesc blend;
        blend.path = SdfPath("/Groom/B");
        blend.type = TfToken("UsdGenWidthBlend");
        blend.inputs = {wl.path, wl.path};
        blend.blend = .5f;
        desc.nodes = {source, wl, blend};
        desc.terminal = blend.path;
        CHECK(RejectsWith(desc, "two distinct graph inputs"));
    }
    {
        auto desc = Base();
        auto source = Source();
        auto wl = Width("/Groom/WL", source.path, .2f);
        auto wr = Width("/Groom/WR", source.path, .8f);
        auto blend = Blend("/Groom/B", wl.path, wr.path, 1.25f);
        desc.nodes = {source, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(RejectsWith(desc, "finite blend"));
    }
    {
        // WidthBlend owns no expressions; it blends proved immutable values.
        auto desc = Base();
        auto source = Source();
        auto wl = Width("/Groom/WL", source.path, .2f);
        auto wr = Width("/Groom/WR", source.path, .8f);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        UsdGenExpressionBinding binding;
        binding.destination = TfToken("blend");
        blend.expressionBindings.push_back(binding);
        desc.nodes = {source, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(!Compiles(desc));
    }
    {
        // Expression-driven Widths refine inside Deform-bearing DAGs (the
        // RBF recipe covers their candidate charging); Width-only DAGs
        // stay on the task-estimate route, and the linear recipe stays
        // literal-only.
        auto desc = Base();
        auto source = Source();
        auto left = Deform("/Groom/DL", source.path);
        auto right = Deform("/Groom/DR", source.path);
        auto wl = Width("/Groom/WL", left.path, .2f);
        auto wr = Width("/Groom/WR", right.path, .8f);
        UsdGenExpressionDesc expression;
        expression.path = SdfPath("/Groom/Expr");
        expression.source = "$value";
        expression.outputs.push_back({TfToken("result"), TfToken("float"),
                                      {expr::ScalarType::Float32, 1, 1, 1, 1, false}});
        desc.expressions.push_back(expression);
        UsdGenExpressionBinding binding;
        binding.expression = expression.path;
        binding.destination = TfToken("width");
        binding.domain = expr::Domain::Groom;
        binding.nativeType = TfToken("float");
        binding.destinationShape = {expr::ScalarType::Float32, 1, 1, 1, 1, false};
        binding.literal = VtValue(1.f);
        wl.expressionBindings.push_back(binding);
        auto blend = Blend("/Groom/B", wl.path, wr.path);
        desc.nodes = {source, left, right, wl, wr, blend};
        desc.terminal = blend.path;
        CHECK(Compiles(desc, &shape, &refinable) &&
              shape == UsdGenExecutionPlanShape::SourceRootedValueDag && refinable);
    }

    // Authoring errors.
    {
        // A single sink is inferred when no terminal is authored.
        auto desc = Base();
        auto source = Source();
        auto width = Width("/Groom/W", source.path);
        desc.nodes = {source, width};
        CHECK(Compiles(desc));
    }
    {
        // Multiple sinks require an explicit terminal.
        auto desc = Base();
        auto source = Source();
        auto first = Width("/Groom/W1", source.path, .2f);
        auto second = Width("/Groom/W2", source.path, .8f);
        desc.nodes = {source, first, second};
        CHECK(RejectsWith(desc, "require an explicit terminal"));
    }
    {
        // Two CurveSources are not a rooted graph.
        auto desc = Base();
        auto first = Source();
        auto second = Source();
        second.path = SdfPath("/Groom/Source2");
        auto width = Width("/Groom/W", first.path);
        desc.nodes = {first, second, width};
        desc.terminal = width.path;
        CHECK(!Compiles(desc));
    }
    {
        // Unknown operators fail at the capability matrix, never silently.
        auto desc = Base();
        auto source = Source();
        UsdGenNodeDesc mystery;
        mystery.path = SdfPath("/Groom/Mystery");
        mystery.type = TfToken("UsdGenClump");
        mystery.inputs = {source.path};
        desc.nodes = {source, mystery};
        desc.terminal = mystery.path;
        CHECK(RejectsWith(desc, "no implementation"));
    }
    {
        // Sources cannot consume upstream geometry in this executor.
        auto desc = Base();
        auto source = Source();
        auto width = Width("/Groom/W", source.path);
        source.inputs = {width.path};
        desc.nodes = {source, width};
        desc.terminal = width.path;
        CHECK(!Compiles(desc));
    }
    {
        // Deform surface must match the source root-binding surface.
        auto desc = Base();
        auto source = Source();
        auto deform = Deform("/Groom/D", source.path);
        deform.surfaces = {SdfPath("/Groom/OtherScalp")};
        desc.nodes = {source, deform};
        desc.terminal = deform.path;
        CHECK(RejectsWith(desc, "must match"));
    }
    {
        // rbfSamples below the solver minimum is rejected at validation.
        auto desc = Base();
        auto source = Source();
        auto deform = Deform("/Groom/D", source.path, 3);
        desc.nodes = {source, deform};
        desc.terminal = deform.path;
        CHECK(!Compiles(desc));
    }
    {
        // Length rejects map inputs; only Width owns the image path.
        auto desc = Base();
        auto source = Source();
        auto length = Length("/Groom/L", source.path);
        length.maps = {SdfPath("/Groom/Map")};
        desc.nodes = {source, length};
        desc.terminal = length.path;
        CHECK(!Compiles(desc));
    }
    return 0;
}
