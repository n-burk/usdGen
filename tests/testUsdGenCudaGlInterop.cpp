// Real Storm buffer allocation/computation and GL rasterization. Only the
// final image is read back; generated geometry never crosses the host.
#include "usdGenImaging/cudaGlComputation.h"
#include "usdGen/executionPipeline.h"
#include "usdGen/graphDesc.h"
#include "usdGen/session.h"
#include "cudaGlFixture.h"
#include "eglctx.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/bufferSpec.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using usdGenImaging::UsdGenCudaGlComputation;
using usdGenTest::MakeCudaGlFixture;
using Semantic = usdGen::UsdGenDeviceChannelSemantic;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (false)

namespace {
constexpr std::array<Semantic,8> channels{Semantic::Points, Semantic::RestPoints,
    Semantic::Widths, Semantic::HairT, Semantic::CurveOffsets,
    Semantic::StableIds, Semantic::RootPrim, Semantic::RootUV};
struct Buffers {
    std::array<HdBufferArrayRangeSharedPtr,8> ranges;
    std::array<std::shared_ptr<UsdGenCudaGlComputation>,8> copies;
};
Buffers Queue(HdStResourceRegistry& registry,
              std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
              bool interleaved = false, bool rejectLastChannel = false) {
    Buffers result;
    for (size_t i = 0; i < channels.size(); ++i) {
        auto copy = std::make_shared<UsdGenCudaGlComputation>(generation, channels[i], TfToken("data"));
        HdBufferSpecVector specs;
        copy->GetBufferSpecs(&specs);
        // Test-only admission failure after other channels may have copied.
        if (rejectLastChannel && i + 1 == channels.size()) {
            specs.back().tupleType = HdTupleType{HdTypeFloatVec3, 1};
        }
        // Same role/spec permits aggregation. Later copies must preserve
        // the earlier range in the same backing GL allocation.
        if (interleaved) {
            specs.insert(specs.begin(), HdBufferSpec(TfToken("aaaPadding"), {HdTypeUInt32Vec4,1}));
            result.ranges[i] = registry.AllocateShaderStorageBufferArrayRange(
                TfToken("usdGenInteropTest"), specs, HdBufferArrayUsageHintBitsStorage);
        } else {
            result.ranges[i] = registry.AllocateNonUniformBufferArrayRange(
                TfToken("usdGenInteropTest"), specs, HdBufferArrayUsageHintBitsStorage);
        }
        registry.AddComputation(result.ranges[i], copy, HdStComputeQueueZero);
        result.copies[i] = std::move(copy);
    }
    return result;
}
GLuint Shader(GLenum type, char const* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint okay = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
    if (!okay) { char log[4096]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); return 0; }
    return shader;
}
GLuint Program() {
    char const* vertex = R"GLSL(#version 450 core
layout(std430,binding=0) readonly buffer P { float p[]; };
layout(std430,binding=1) readonly buffer R { float r[]; };
layout(std430,binding=2) readonly buffer W { float w[]; };
layout(std430,binding=3) readonly buffer T { float t[]; };
layout(std430,binding=4) readonly buffer O { uint o[]; };
layout(std430,binding=5) readonly buffer I { uvec2 ids[]; };
layout(std430,binding=6) readonly buffer F { int f[]; };
layout(std430,binding=7) readonly buffer U { float uv[]; };
uniform int base[8];
uniform int stride[8];
uniform float expectedWidth;
out float hairWidth;
flat out int valid;
bool near(float a,float b) { return abs(a-b)<0.00001; }
void main() {
    bool ok = o[base[4]]==0u && o[base[4]+stride[4]]==2u && o[base[4]+2*stride[4]]==5u;
    ok = ok && all(equal(ids[base[5]],uvec2(0x0000005bu,0x12345678u)))
            && all(equal(ids[base[5]+stride[5]],uvec2(0x00000025u,0xabcdef01u)));
    ok = ok && f[base[6]]==4 && f[base[6]+stride[6]]==8;
    ok = ok && near(uv[base[7]],.1) && near(uv[base[7]+1],.2)
            && near(uv[base[7]+stride[7]],.3) && near(uv[base[7]+stride[7]+1],.4);
    for(int j=0;j<5;++j) {
        float x = j<2 ? -.5 : .5;
        float y = j==0 || j==2 ? -.6 : j==3 ? 0.0 : .6;
        float h = j==0 || j==2 ? 0.0 : j==3 ? .5 : 1.0;
        ok = ok && near(p[base[0]+j*stride[0]],x) && near(p[base[0]+j*stride[0]+1],y)
                && near(p[base[0]+j*stride[0]+2],0.0) && near(t[base[3]+j*stride[3]],h)
                && near(w[base[2]+j*stride[2]],expectedWidth);
        for(int k=0;k<3;++k) ok = ok && near(r[base[1]+j*stride[1]+k],p[base[0]+j*stride[0]+k]);
    }
    // Curve endpoints come from GPU topology, not a host offsets mirror.
    int curve=gl_VertexID/2;
    uint index = gl_VertexID%2==0 ? o[base[4]+curve*stride[4]] : o[base[4]+(curve+1)*stride[4]]-1u;
    // Clamp only to contain a broken test input; validation still turns red.
    index=min(index,4u);
    int v=base[0]+int(index)*stride[0];
    gl_Position=vec4(p[v],p[v+1],p[v+2],1.0);
    hairWidth=w[base[2]+int(index)*stride[2]];
    valid=ok?1:0;
})GLSL";
    char const* geometry = R"GLSL(#version 450 core
layout(lines) in;
layout(triangle_strip,max_vertices=4) out;
in float hairWidth[];
flat in int valid[];
flat out int good;
void main() {
    good=valid[0]&valid[1];
    for(int end=0;end<2;++end) for(int side=-1;side<=1;side+=2) {
        gl_Position=gl_in[end].gl_Position+vec4(float(side)*hairWidth[end]*.5,0,0,0);
        EmitVertex();
    }
    EndPrimitive();
})GLSL";
    char const* fragment = R"GLSL(#version 450 core
flat in int good;
out vec4 color;
void main() { color=good==1?vec4(0,1,0,1):vec4(1,0,0,1); }
)GLSL";
    GLuint v=Shader(GL_VERTEX_SHADER,vertex), g=Shader(GL_GEOMETRY_SHADER,geometry), f=Shader(GL_FRAGMENT_SHADER,fragment);
    if (!v || !g || !f) return 0;
    GLuint program=glCreateProgram();
    glAttachShader(program,v); glAttachShader(program,g); glAttachShader(program,f); glLinkProgram(program);
    glDeleteShader(v); glDeleteShader(g); glDeleteShader(f);
    GLint okay=0; glGetProgramiv(program,GL_LINK_STATUS,&okay);
    if (!okay) { char log[4096]; glGetProgramInfoLog(program,sizeof(log),nullptr,log); std::fprintf(stderr,"%s\n",log); return 0; }
    return program;
}
int Draw(Buffers const& buffers, GLuint program, float width) {
    std::array<GLint,8> offsets{};
    std::array<GLint,8> strides{};
    for(size_t i=0;i<channels.size();++i) {
        auto bar=std::dynamic_pointer_cast<HdStBufferArrayRange>(buffers.ranges[i]);
        if (!bar) return -1;
        auto resource=bar->GetResource(TfToken("data"));
        auto* buffer=resource ? dynamic_cast<HgiGLBuffer*>(resource->GetHandle().Get()) : nullptr;
        if (!buffer) return -1;
        // Bind the entire allocation (BAR byte offsets need not meet SSBO
        // binding alignment). Shader indexing applies the range's offset.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,static_cast<GLuint>(i),buffer->GetBufferId());
        offsets[i]=(bar->GetByteOffset(TfToken("data"))+resource->GetOffset())/(i==5?8:4);
        strides[i]=resource->GetStride()/(i==5?8:4);
    }
    glUseProgram(program);
    glUniform1iv(glGetUniformLocation(program,"base[0]"),8,offsets.data());
    glUniform1iv(glGetUniformLocation(program,"stride[0]"),8,strides.data());
    glUniform1f(glGetUniformLocation(program,"expectedWidth"),width);
    glViewport(0,0,64,64); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_LINES,0,4);
    std::array<unsigned char,64*64*4> pixels{};
    glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    if(glGetError()!=GL_NO_ERROR) return -1;
    int green=0, red=0;
    for(size_t i=0;i<pixels.size();i+=4) { if(pixels[i]>128) ++red; if(pixels[i+1]>128) ++green; }
    std::printf("width %.3f: green=%d red=%d\n",width,green,red);
    return red ? -1 : green;
}

}

// Version acceptance, last-good visibility and renderer retirement through
// the bridge, end to end with traces. v1 publishes, v2 supersedes into
// fresh ranges, a failed v3 never becomes displayable, and an explicit
// render fence retires each displayed version before its destruction.
static int Lifecycle() {
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    HdStRenderDelegate delegate;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi*>(&hgi))};
    delegate.SetDrivers({&driver});
    auto registryOwner = std::dynamic_pointer_cast<HdStResourceRegistry>(delegate.GetResourceRegistry());
    CHECK(registryOwner);
    auto& registry = *registryOwner;
    auto first = MakeCudaGlFixture(.1f), second = MakeCudaGlFixture(.2f);
    CHECK(first && second);
    std::weak_ptr<const usdGen::UsdGenDeviceOwner> firstOwner = first->Owner();
    std::weak_ptr<const usdGen::UsdGenDeviceOwner> secondOwner = second->Owner();
    auto a = Queue(registry, first);
    registry.Commit();
    for (auto const& copy : a.copies) {
        if (!copy->Succeeded()) std::fprintf(stderr, "lifecycle v1: %s\n", copy->Error().c_str());
        CHECK(copy->Succeeded());
    }
    GLuint program = Program(), vao = 0;
    CHECK(program);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    int v1pixels = Draw(a, program, .1f);
    CHECK(v1pixels > 100);
    std::fprintf(stderr, "lifecycle: version 1 published (%d px)\n", v1pixels);
    auto b = Queue(registry, second);
    registry.Commit();
    for (auto const& copy : b.copies) {
        if (!copy->Succeeded()) std::fprintf(stderr, "lifecycle v2: %s\n", copy->Error().c_str());
        CHECK(copy->Succeeded());
    }
    int v2pixels = Draw(b, program, .2f);
    CHECK(v2pixels > v1pixels);
    std::fprintf(stderr, "lifecycle: version 2 accepted (%d px), version 1 superseded\n", v2pixels);
    // Destroying the superseded version cannot disturb the displayed one:
    // ranges are Storm-owned and generations are independently retained.
    first.reset();
    for (auto& copy : a.copies) copy.reset();
    CHECK(firstOwner.expired());
    CHECK(Draw(b, program, .2f) == v2pixels);
    std::fprintf(stderr, "lifecycle: version 1 retired, version 2 still displayed\n");
    // A failed admission never becomes displayable: the caller keeps
    // showing the last good version.
    auto third = MakeCudaGlFixture(.3f);
    CHECK(third);
    auto c = Queue(registry, third, false, true);
    registry.Commit();
    bool sawFailure = false;
    for (auto const& copy : c.copies) {
        if (!copy->Succeeded()) {
            sawFailure = true;
            CHECK(!copy->Error().empty());
        }
    }
    CHECK(sawFailure);
    CHECK(Draw(b, program, .2f) == v2pixels);
    std::fprintf(stderr, "lifecycle: failed version 3 rejected, last-good version 2 displayed\n");
    // Renderer retirement: fence the displayed draw before destroying
    // anything the GPU may still read.
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(fence);
    CHECK(glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 10000000000ull) != GL_TIMEOUT_EXPIRED);
    glDeleteSync(fence);
    second.reset();
    for (auto& copy : b.copies) copy.reset();
    for (auto& copy : c.copies) copy.reset();
    third.reset();
    CHECK(secondOwner.expired());
    std::fprintf(stderr, "lifecycle: retire fence proved, all versions destroyed\n");
    glBindVertexArray(0); glDeleteVertexArrays(1, &vao); glUseProgram(0); glDeleteProgram(program);
    CHECK(errors.IsClean());
    std::puts("testUsdGenCudaGlLifecycle: PASS (versions, last-good, retirement fence)");
    return 0;
}

// Tile-scoped transfers carry per-tile channels and global offsets exactly:
// a tile transfer must equal the corresponding slice of the whole-generation
// transfer, and an out-of-range tile id must fail closed without touching
// Storm state.
static usdGen::UsdGenGraphDesc TileDesc() {
    usdGen::UsdGenGraphDesc desc;
    desc.description = SdfPath("/TileHandoff");
    desc.executionBackend = usdGen::UsdGenExecutionBackend::Cuda;
    desc.defaultWidth = .025f;
    usdGen::UsdGenSurfaceDesc scalp;
    scalp.path = SdfPath("/Scalp");
    scalp.worldMatrix.SetIdentity();
    scalp.restPoints = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    scalp.points = scalp.restPoints;
    scalp.faceVertexCounts = {3};
    scalp.faceVertexIndices = {0, 1, 2};
    desc.surfaces.push_back(scalp);
    usdGen::UsdGenCurveSetDesc hair;
    hair.path = SdfPath("/Hair");
    hair.role = usdGen::UsdGenRole::Curves;
    hair.curveRole = TfToken("hair");
    hair.type = TfToken("cubic");
    hair.basis = TfToken("bspline");
    hair.wrap = TfToken("pinned");
    hair.curveVertexCounts.assign(600, 2);
    hair.curveId.resize(600);
    hair.skinPrim.assign(600, 0);
    hair.skinPrimUv.assign(600, GfVec2f(.25f, .25f));
    hair.points.resize(1200);
    for (size_t curve = 0; curve != 600; ++curve) {
        float const x = float(curve) * .001f;
        hair.curveId[curve] = curve;
        hair.points[2 * curve] = GfVec3f(x, 0, 0);
        hair.points[2 * curve + 1] = GfVec3f(x, 2.f, 0);
    }
    hair.rest = hair.points;
    desc.curveSets.push_back(hair);
    usdGen::UsdGenNodeDesc source;
    source.path = SdfPath("/Ops/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {hair.path};
    source.surfaces = {scalp.path};
    usdGen::UsdGenNodeDesc widthNode;
    widthNode.path = SdfPath("/Ops/Width");
    widthNode.type = TfToken("UsdGenWidth");
    widthNode.inputs = {source.path};
    widthNode.params.push_back({TfToken("width"), VtValue(.5f), false});
    desc.nodes = {source, widthNode};
    desc.terminal = widthNode.path;
    return desc;
}

static int TileTransfers() {
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    HdStRenderDelegate delegate;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi*>(&hgi))};
    delegate.SetDrivers({&driver});
    auto registryOwner = std::dynamic_pointer_cast<HdStResourceRegistry>(delegate.GetResourceRegistry());
    CHECK(registryOwner);
    auto& registry = *registryOwner;
    usdGen::UsdGenSession session;
    session.SetDevicePublicationEnabled(true);
    session.SetGraphDesc(TileDesc());
    auto published = session.Commit(1, usdGen::UsdGenCommitReason::SetTime);
    if (!published || !published->device || session.LastDiagnostics().HasErrors()) {
        for (auto const& error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "tile session: %s\n", error.c_str());
        return 1;
    }
    auto generation = published->device;
    auto const& tiles = generation->Geometry().tiles;
    CHECK(tiles.size() >= 2);
    auto const& tail = tiles.back();
    CHECK(tail.pointCount != 0 && tail.firstPoint != 0);
    std::fprintf(stderr, "tiles: count=%zu tail curves=%llu points=%llu firstPoint=%llu\n",
        tiles.size(), static_cast<unsigned long long>(tail.curveCount),
        static_cast<unsigned long long>(tail.pointCount),
        static_cast<unsigned long long>(tail.firstPoint));
    // Whole-generation reference transfers.
    auto whole = Queue(registry, generation);
    registry.Commit();
    for (auto const& copy : whole.copies) {
        if (!copy->Succeeded()) std::fprintf(stderr, "tile whole: %s\n", copy->Error().c_str());
        CHECK(copy->Succeeded());
    }
    // Tile-scoped transfers must carry the identical bytes.
    auto transferTile = [&](Semantic semantic, uint32_t tileId,
                            Buffers* out) {
        auto copy = std::make_shared<UsdGenCudaGlComputation>(
            generation, semantic, TfToken("data"), tileId);
        HdBufferSpecVector specs;
        copy->GetBufferSpecs(&specs);
        auto range = registry.AllocateNonUniformBufferArrayRange(
            TfToken("usdGenInteropTileTest"), specs, HdBufferArrayUsageHintBitsStorage);
        registry.AddComputation(range, copy, HdStComputeQueueZero);
        out->ranges[0] = range;
        out->copies[0] = std::move(copy);
    };
    auto compareArrays = [&](HdBufferArrayRangeSharedPtr const& wholeRange,
                             HdBufferArrayRangeSharedPtr const& tileRange,
                             uint64_t wholeBaseFloats, uint64_t count) {
        // Compares tile floats against the whole-generation slice in-shader;
        // generated geometry never crosses the host in this proof.
        static GLuint program = 0;
        static GLuint vao = 0;
        if (!program) {
            char const* vertex = R"GLSL(#version 450 core
layout(std430,binding=0) readonly buffer A { float a[]; };
layout(std430,binding=1) readonly buffer B { float b[]; };
uniform int baseA; uniform int baseB; uniform int count;
flat out int good;
void main() {
    int i = gl_VertexID;
    good = 1;
    if (i < count) good = (a[baseA+i] == b[baseB+i]) ? 1 : 0;
    float x = -1.0 + 2.0 * float(i % 64) / 64.0 + 1.0 / 64.0;
    float y = -1.0 + 2.0 * float(i / 64) / 64.0 + 1.0 / 64.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    gl_PointSize = 1.0;
})GLSL";
            char const* fragment = R"GLSL(#version 450 core
flat in int good;
out vec4 color;
void main() { color = good==1 ? vec4(0,1,0,1) : vec4(1,0,0,1); }
)GLSL";
            GLuint v = Shader(GL_VERTEX_SHADER, vertex), f = Shader(GL_FRAGMENT_SHADER, fragment);
            CHECK(v && f);
            program = glCreateProgram();
            CHECK(program);
            glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
            glDeleteShader(v); glDeleteShader(f);
            GLint okay = 0; glGetProgramiv(program, GL_LINK_STATUS, &okay);
            CHECK(okay);
            glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        }
        auto wholeBar = std::dynamic_pointer_cast<HdStBufferArrayRange>(wholeRange);
        auto tileBar = std::dynamic_pointer_cast<HdStBufferArrayRange>(tileRange);
        CHECK(wholeBar && tileBar);
        auto wholeResource = wholeBar->GetResource(TfToken("data"));
        auto tileResource = tileBar->GetResource(TfToken("data"));
        CHECK(wholeResource && tileResource);
        auto* wholeBuffer = dynamic_cast<HgiGLBuffer*>(wholeResource->GetHandle().Get());
        auto* tileBuffer = dynamic_cast<HgiGLBuffer*>(tileResource->GetHandle().Get());
        CHECK(wholeBuffer && tileBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, wholeBuffer->GetBufferId());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, tileBuffer->GetBufferId());
        glUseProgram(program);
        GLint baseA = GLint((wholeBar->GetByteOffset(TfToken("data")) + wholeResource->GetOffset()) / 4 +
            wholeBaseFloats);
        GLint baseB = GLint((tileBar->GetByteOffset(TfToken("data")) + tileResource->GetOffset()) / 4);
        glUniform1i(glGetUniformLocation(program, "baseA"), baseA);
        glUniform1i(glGetUniformLocation(program, "baseB"), baseB);
        glUniform1i(glGetUniformLocation(program, "count"), GLint(count));
        glViewport(0, 0, 64, 64); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_POINTS, 0, GLsizei(count));
        std::array<unsigned char, 64*64*4> pixels{};
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (glGetError() != GL_NO_ERROR) return -1;
        int green = 0;
        for (size_t i = 0; i != pixels.size(); i += 4) if (pixels[i+1] > 128) ++green;
        return green;
    };
    {
        Buffers tilePoints, tileWidths;
        transferTile(Semantic::Points, 1, &tilePoints);
        transferTile(Semantic::Widths, 1, &tileWidths);
            registry.Commit();
            for (auto* set : {&tilePoints, &tileWidths})
            for (auto const& copy : set->copies) {
                if (!copy) continue;
                if (!copy->Succeeded())
                    std::fprintf(stderr, "tile transfer: %s\n", copy->Error().c_str());
                CHECK(copy->Succeeded());
            }
        uint64_t const pointFloats = tail.pointCount * 3;
        int pointGreen = compareArrays(whole.ranges[0], tilePoints.ranges[0],
                                       tail.firstPoint * 3, pointFloats);
        std::fprintf(stderr, "tiles: points green=%d/%d\n", pointGreen, int(pointFloats));
        CHECK(pointGreen == int(pointFloats));
        int widthGreen = compareArrays(whole.ranges[2], tileWidths.ranges[0],
                                       tail.firstPoint, tail.pointCount);
        std::fprintf(stderr, "tiles: widths green=%d/%llu\n", widthGreen,
                     static_cast<unsigned long long>(tail.pointCount));
        CHECK(widthGreen == int(tail.pointCount));
        std::fprintf(stderr, "tiles: tile-1 points+widths match whole-generation slice\n");
    }
    {
        // An out-of-range tile id fails closed: no crash, no hang, an error,
        // and the valid ranges still draw.
        auto bad = std::make_shared<UsdGenCudaGlComputation>(
            generation, Semantic::Points, TfToken("data"), 999u);
        HdBufferSpecVector specs;
        bad->GetBufferSpecs(&specs);
        auto range = registry.AllocateNonUniformBufferArrayRange(
            TfToken("usdGenInteropTileTest"), specs, HdBufferArrayUsageHintBitsStorage);
        registry.AddComputation(range, bad, HdStComputeQueueZero);
        registry.Commit();
        CHECK(!bad->Succeeded() && !bad->Error().empty());
        std::fprintf(stderr, "tiles: out-of-range tile id fails closed\n");
    }
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(fence);
    CHECK(glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 10000000000ull) != GL_TIMEOUT_EXPIRED);
    glDeleteSync(fence);
    generation.reset();
    CHECK(errors.IsClean());
    std::puts("testUsdGenCudaGlTiles: PASS (tile slices match, bad tile fails closed, fence proved)");
    return 0;
}

int main(int argc, char** argv) {
    TfErrorMark errors;
    if (argc == 2 && std::strcmp(argv[1], "--lifecycle") == 0) return Lifecycle();
    if (argc == 2 && std::strcmp(argv[1], "--tiles") == 0) return TileTransfers();
    if(!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    // Let the SDK construct its own registry. Its public header has a
    // MaterialX-conditional member but the SDK does not export that feature
    // macro to consumers, so allocating sizeof(HdStResourceRegistry) here
    // would assume an ABI/layout which may differ from the shared library.
    HdStRenderDelegate delegate;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi*>(&hgi))};
    delegate.SetDrivers({&driver});
    auto registryOwner = std::dynamic_pointer_cast<HdStResourceRegistry>(delegate.GetResourceRegistry());
    CHECK(registryOwner);
    auto& registry = *registryOwner;
    auto first=MakeCudaGlFixture(.1f), second=MakeCudaGlFixture(.2f);
    CHECK(first && second);
    std::weak_ptr<const usdGen::UsdGenDeviceOwner> retained=first->Owner();
    auto a=Queue(registry,first), b=Queue(registry,second);
    auto c=Queue(registry,MakeCudaGlFixture(.3f),true);
    first.reset(); second.reset();
    CHECK(!retained.expired());
    registry.Commit();
    for(auto const* set : {&a,&b,&c}) for(auto const& copy:set->copies) {
        if(!copy->Succeeded()) std::fprintf(stderr,"interop: %s\n",copy->Error().c_str());
        CHECK(copy->Succeeded());
    }
    GLuint program=Program(), vao=0;
    CHECK(program);
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    int thin=Draw(a,program,.1f), thick=Draw(b,program,.2f);
    CHECK(thin>100 && thick>thin);
    CHECK(Draw(c,program,.3f)>thick);
    auto interleaved=std::dynamic_pointer_cast<HdStBufferArrayRange>(c.ranges[0]);
    CHECK(interleaved->GetResource(TfToken("data"))->GetOffset()>0);
    CHECK(interleaved->GetResource(TfToken("data"))->GetStride()>12);
    auto aPoints=std::dynamic_pointer_cast<HdStBufferArrayRange>(a.ranges[0]);
    auto bPoints=std::dynamic_pointer_cast<HdStBufferArrayRange>(b.ranges[0]);
    CHECK(aPoints->GetResource(TfToken("data"))->GetHandle()==bPoints->GetResource(TfToken("data"))->GetHandle());
    CHECK(aPoints->GetByteOffset(TfToken("data"))!=bPoints->GetByteOffset(TfToken("data")));
    CHECK(Draw(a,program,.1f)==thin); // no adjacent-range corruption
    {
        UsdGenCudaGlComputation missing({},Semantic::Points,TfToken("data"));
        HdBufferSpecVector specs;
        missing.GetBufferSpecs(&specs);
        CHECK(specs.empty() && missing.GetNumOutputElements()==0);
        missing.Execute(a.ranges[0],&registry);
        CHECK(!missing.Succeeded() && !missing.Error().empty());
        auto validationSource=MakeCudaGlFixture(.8f);
        CHECK(validationSource);
        UsdGenCudaGlComputation unsupported(validationSource,Semantic::Generic,TfToken("data"));
        unsupported.Execute(a.ranges[0],&registry);
        CHECK(!unsupported.Succeeded());
        UsdGenCudaGlComputation mismatch(validationSource,Semantic::Widths,TfToken("data"));
        mismatch.Execute(a.ranges[0],&registry); // widths cannot overwrite points
        CHECK(!mismatch.Succeeded() && !mismatch.Error().empty());
        mismatch.Execute({},&registry);
        CHECK(!mismatch.Succeeded());
        usdGen::UsdGenExecutionRuntime runtime(2);
        usdGen::UsdGenExecutionPipeline pipeline(runtime);
        pipeline.InvokeOwner([&] { mismatch.Execute(a.ranges[2],&registry); });
        CHECK(!mismatch.Succeeded() && mismatch.Error().find("external graphics")!=std::string::npos);
        pipeline.Shutdown();
        CHECK(Draw(a,program,.1f)==thin); // rejected work did not alter destination
    }
    for(auto& copy:a.copies) copy.reset();
    for(auto& copy:b.copies) copy.reset();
    for(auto& copy:c.copies) copy.reset();
    CHECK(retained.expired());
    CHECK(Draw(a,program,.1f)==thin); // GL allocation owns its D2D result
    glBindVertexArray(0); glDeleteVertexArrays(1,&vao); glUseProgram(0); glDeleteProgram(program);
    CHECK(errors.IsClean());
    std::puts("testUsdGenCudaGlInterop: PASS (Storm BAR -> GL draw; not full BasisCurves publication)");
    return 0;
}
