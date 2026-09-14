#include "pxr/base/gf/vec2f.h"
#include "cudaSurfaceInput.h"
#include <cstdio>
#include <limits>

using namespace usdGen;

int main() {
    UsdGenSurfaceDesc source;
    source.path = SdfPath("/mesh");
    source.worldMatrix.SetIdentity();
    source.restPoints = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}, {0,0,1}};
    source.points = source.restPoints;
    source.faceVertexCounts = {3, 4};
    source.faceVertexIndices = {0, 1, 2, 0, 2, 3, 4};
    CudaSurfacePrepared prepared;
    std::vector<std::string> diagnostics;
    if (PrepareCudaSurface(source, 8, 3, &prepared, &diagnostics) != CudaSurfacePreparationStatus::Ok) {
        std::fprintf(stderr, "valid surface rejected\n"); return 1;
    }
    if (prepared.restPoints.size() != 5 || prepared.currentPoints.size() != 5 ||
        prepared.faceOffsets.size() != 3 || prepared.faceOffsets.back() != 7 ||
        prepared.faceVertexIndices.size() != 7 || prepared.sampleBudget != 8 ||
        prepared.algorithmVersion != 3) {
        std::fprintf(stderr, "prepared surface shape mismatch\n"); return 1;
    }

    auto normalSource = source;
    normalSource.restNormals = {{0, 0, 1}};
    normalSource.restNormalDomain = UsdGenSurfaceNormalDomain::Constant;
    CudaSurfacePrepared normalPrepared;
    if (PrepareCudaSurface(normalSource, 8, 3, &normalPrepared, nullptr) !=
            CudaSurfacePreparationStatus::Ok ||
        normalPrepared.restNormalDomain != UsdGenSurfaceNormalDomain::Constant ||
        normalPrepared.restNormals.size() != 1 ||
        RestBindingMatches(prepared, normalPrepared)) {
        std::fprintf(stderr, "constant rest normal binding was not retained\n"); return 1;
    }
    normalSource.restNormals = {{0, 0, 1}, {0, 1, 0}};
    normalSource.restNormalDomain = UsdGenSurfaceNormalDomain::Uniform;
    if (PrepareCudaSurface(normalSource, 8, 3, &normalPrepared, nullptr) !=
            CudaSurfacePreparationStatus::Ok ||
        normalPrepared.restNormals.size() != 2 ||
        normalPrepared.restNormalDomain != UsdGenSurfaceNormalDomain::Uniform) {
        std::fprintf(stderr, "uniform rest normal binding was rejected\n"); return 1;
    }
    normalSource.restNormals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1},
                                {0, 0, 1}, {0, 0, 1}};
    normalSource.restNormalDomain = UsdGenSurfaceNormalDomain::Vertex;
    if (PrepareCudaSurface(normalSource, 8, 3, &normalPrepared, nullptr) !=
            CudaSurfacePreparationStatus::Ok ||
        normalPrepared.restNormals.size() != 5 ||
        normalPrepared.restNormalDomain != UsdGenSurfaceNormalDomain::Vertex) {
        std::fprintf(stderr, "vertex rest normal binding was rejected\n"); return 1;
    }
    normalSource.restNormals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1},
                                {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    normalSource.restNormalDomain = UsdGenSurfaceNormalDomain::FaceVarying;
    if (PrepareCudaSurface(normalSource, 8, 3, &normalPrepared, nullptr) !=
            CudaSurfacePreparationStatus::Ok ||
        normalPrepared.restNormals.size() != 7 ||
        normalPrepared.restNormalDomain != UsdGenSurfaceNormalDomain::FaceVarying) {
        std::fprintf(stderr, "face-varying rest normal binding was rejected\n"); return 1;
    }

    auto animated = source;
    animated.points[0] = GfVec3f(.25f, 0, 0);
    CudaSurfacePrepared animatedPrepared;
    if (PrepareCudaSurface(animated, 8, 3, &animatedPrepared, nullptr) != CudaSurfacePreparationStatus::Ok ||
        !RestBindingMatches(prepared, animatedPrepared) ||
        animatedPrepared.currentPoints[0].x != .25f) {
        std::fprintf(stderr, "current animation changed persistent rest key\n"); return 1;
    }
    auto changedTopology = source;
    changedTopology.faceVertexIndices[0] = 4;
    CudaSurfacePrepared topologyPrepared;
    if (PrepareCudaSurface(changedTopology, 8, 3, &topologyPrepared, nullptr) != CudaSurfacePreparationStatus::Ok ||
        RestBindingMatches(prepared, topologyPrepared)) {
        std::fprintf(stderr, "topology did not invalidate rest key\n"); return 1;
    }
    CudaSurfacePrepared budgetPrepared;
    if (PrepareCudaSurface(source, 9, 3, &budgetPrepared, nullptr) != CudaSurfacePreparationStatus::Ok ||
        RestBindingMatches(prepared, budgetPrepared)) {
        std::fprintf(stderr, "sample budget did not invalidate rest key\n"); return 1;
    }

    CudaSurfacePrepared sentinel = prepared;
    auto malformed = source;
    malformed.faceVertexIndices[0] = 99;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, &diagnostics) == CudaSurfacePreparationStatus::Ok ||
        prepared.restPoints.size() != sentinel.restPoints.size() ||
        prepared.faceOffsets.size() != sentinel.faceOffsets.size() ||
        prepared.faceOffsets.back() != sentinel.faceOffsets.back()) {
        std::fprintf(stderr, "malformed surface was accepted or damaged prior output\n"); return 1;
    }
    malformed = source;
    malformed.subsetFaces = {0};
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) != CudaSurfacePreparationStatus::UnsupportedFeature) return 1;
    malformed = source;
    malformed.restFromCurrentPoints = true;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) != CudaSurfacePreparationStatus::UnsupportedFeature) return 1;
    malformed = source;
    malformed.worldMatrix[0][0] = 2.0;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) != CudaSurfacePreparationStatus::UnsupportedFeature) return 1;
    if (PrepareCudaSurface(source, 3, 3, &prepared, nullptr) != CudaSurfacePreparationStatus::InvalidArgument) return 1;
    malformed = source;
    malformed.faceVertexCounts[0] = 5;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) != CudaSurfacePreparationStatus::InvalidTopology) return 1;
    malformed = source;
    malformed.restNormals = {{0, 0, 1}};
    malformed.restNormalDomain = UsdGenSurfaceNormalDomain::None;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) !=
            CudaSurfacePreparationStatus::InvalidArgument ||
        prepared.restPoints.size() != sentinel.restPoints.size()) {
        std::fprintf(stderr, "normal None/cardinality mismatch was accepted or damaged output\n"); return 1;
    }
    malformed = source;
    malformed.restNormals = {{0, 0, 1}};
    malformed.restNormalDomain = UsdGenSurfaceNormalDomain::Uniform;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) !=
        CudaSurfacePreparationStatus::InvalidArgument) return 1;
    malformed = source;
    malformed.restNormals = {{std::numeric_limits<float>::quiet_NaN(), 0, 1}};
    malformed.restNormalDomain = UsdGenSurfaceNormalDomain::Constant;
    if (PrepareCudaSurface(malformed, 8, 3, &prepared, nullptr) !=
        CudaSurfacePreparationStatus::NonFiniteInput) return 1;
    return 0;
}
