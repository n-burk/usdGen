// usdGenTonic — C ABI implementation (P0 model + P1 commit + P2 graph/bake).
#include "usdGenTonic/tonicApi.h"
#include "usdGenTonic/tonicBake.h"
#include "usdGenTonic/tonicCommit.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicRegistry.h"
#include "usdGenTonic/tonicTransport.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

thread_local std::string _tlError;

void _SetError(char const *what)
{
    _tlError = what ? what : "unknown tonic error";
}

struct TonicModelContextImpl {
    usdGenTonic::TonicModel model;
    usdGenTonic::TonicPinnedStaging positions;
    usdGenTonic::TonicPinnedStaging normals;
    // The registry id this model was created under (plan/18 §2.1). The
    // scene indices find the model through it; without it a tool's edits
    // never reach the viewport.
    int modelId = 0;

    TonicModelContextImpl()
    {
        modelId = usdGenTonic::TonicRegistry::Get().Register(&model);
    }
    ~TonicModelContextImpl()
    {
        usdGenTonic::TonicRegistry::Get().Unregister(modelId);
    }
};

TonicModelContextImpl *_Impl(TonicModelContext *ctx)
{
    return reinterpret_cast<TonicModelContextImpl *>(ctx);
}

TonicModelContextImpl const *_Impl(TonicModelContext const *ctx)
{
    return reinterpret_cast<TonicModelContextImpl const *>(ctx);
}

struct TonicCommitterContextImpl {
    std::unique_ptr<usdGenTonic::TonicCommitter> committer;
};

TonicCommitterContextImpl *_CImpl(TonicCommitterContext *cc)
{
    return reinterpret_cast<TonicCommitterContextImpl *>(cc);
}

TonicCommitterContextImpl const *_CImpl(TonicCommitterContext const *cc)
{
    return reinterpret_cast<TonicCommitterContextImpl const *>(cc);
}

struct TonicBakeContextImpl {
    usdGenTonic::TonicModel *model = nullptr;  // non-owning (outlives us)
    std::unique_ptr<usdGenTonic::TonicBakeWorker> worker;
    std::string outDir;
    std::string baseName = "regionMap";
    int resOverride = -1;
    int levelCount = 1;
};

TonicBakeContextImpl *_BImpl(TonicBakeContext *bake)
{
    return reinterpret_cast<TonicBakeContextImpl *>(bake);
}

TonicBakeContextImpl const *_BImpl(TonicBakeContext const *bake)
{
    return reinterpret_cast<TonicBakeContextImpl const *>(bake);
}

// Locate (faceId, u, v) on the model's scalp (positions via
// TonicFacePosition, normals from the face frame).
bool _Locate(usdGenTonic::TonicModel &model, int faceId, float u, float v,
             usdGenTonic::TonicHit *hit)
{
    std::shared_ptr<usdGenTonic::TonicScalpMesh const> scalp =
        model.GetScalp();
    if (!scalp || !scalp->finalized || !hit) {
        return false;
    }
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (!usdGenTonic::TonicFacePosition(*scalp, faceId, u, v, &px, &py, &pz)) {
        return false;
    }
    if (faceId < 0 ||
        size_t(faceId) >= scalp->faceVertexCounts.size()) {
        return false;
    }
    hit->hit = true;
    hit->faceId = faceId;
    hit->u = u;
    hit->v = v;
    hit->px = px;
    hit->py = py;
    hit->pz = pz;
    hit->nx = scalp->faceNormals[size_t(faceId) * 3 + 0];
    hit->ny = scalp->faceNormals[size_t(faceId) * 3 + 1];
    hit->nz = scalp->faceNormals[size_t(faceId) * 3 + 2];
    return true;
}

void _WriteHit(usdGenTonic::TonicHit const &hit, int *outHit, int *outFace,
               float outUV[2], float outP[3], float outN[3])
{
    if (outHit) {
        *outHit = hit.hit ? 1 : 0;
    }
    if (!hit.hit) {
        return;
    }
    if (outFace) {
        *outFace = hit.faceId;
    }
    if (outUV) {
        outUV[0] = hit.u;
        outUV[1] = hit.v;
    }
    if (outP) {
        outP[0] = hit.px;
        outP[1] = hit.py;
        outP[2] = hit.pz;
    }
    if (outN) {
        outN[0] = hit.nx;
        outN[1] = hit.ny;
        outN[2] = hit.nz;
    }
}

} // namespace

extern "C" {

int
Tonic_Create(TonicModelContext **outCtx)
{
    try {
        if (!outCtx) {
            _SetError("Tonic_Create: outCtx is null");
            return TONIC_ERROR;
        }
        *outCtx = reinterpret_cast<TonicModelContext *>(
            new (std::nothrow) TonicModelContextImpl());
        if (!*outCtx) {
            _SetError("Tonic_Create: allocation failed");
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Create: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_Destroy(TonicModelContext *ctx)
{
    try {
        delete _Impl(ctx);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Destroy: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BuildTestTube(TonicModelContext *ctx, int rings, int ringVerts,
                    float radius, float length)
{
    try {
        if (!ctx) {
            _SetError("Tonic_BuildTestTube: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeShape shape;
        if (rings > 0) {
            shape.rings = rings;
        }
        if (ringVerts > 0) {
            shape.ringVerts = ringVerts;
        }
        if (radius > 0.0f) {
            shape.radius = radius;
        }
        if (length > 0.0f) {
            shape.length = length;
        }
        if (!_Impl(ctx)->model.BuildTestTube(shape)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BuildTestTube: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MoveCenterRing(TonicModelContext *ctx, int ring, float dx, float dz)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MoveCenterRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MoveCenterRing(ring, dx, dz)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MoveCenterRing: unknown exception");
        return TONIC_ERROR;
    }
}

unsigned long long
Tonic_GetVersion(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return _Impl(ctx)->model.GetVersion();
    } catch (...) {
        return 0;
    }
}

int
Tonic_TakeDirty(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return int(_Impl(ctx)->model.TakeDirty());
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetVertexCount(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        usdGenTonic::TonicModel::HostTubeMesh const &mesh =
            _Impl(ctx)->model.GetHostMesh();
        return int(mesh.positions.size() / 3);
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetQuadCount(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        usdGenTonic::TonicModel::HostTubeMesh const &mesh =
            _Impl(ctx)->model.GetHostMesh();
        return int(mesh.faceVertexCounts.size());
    } catch (...) {
        return 0;
    }
}

int
Tonic_ReadTubePoints(TonicModelContext *ctx, float *outXYZ, int xyzLen)
{
    try {
        if (!ctx || !outXYZ) {
            _SetError("Tonic_ReadTubePoints: null argument");
            return TONIC_ERROR;
        }
        TonicModelContextImpl *impl = _Impl(ctx);
        usdGenTonic::TonicStagedTubeMesh staged;
        if (!usdGenTonic::TonicStageTubeMesh(impl->model, &impl->positions,
                                             &impl->normals, &staged)) {
            _SetError("Tonic_ReadTubePoints: no tube staged");
            return TONIC_ERROR;
        }
        int const need = int(staged.points.size()) * 3;
        if (xyzLen < need) {
            _SetError("Tonic_ReadTubePoints: output too small");
            return TONIC_ERROR;
        }
        std::memcpy(outXYZ, staged.points.data(),
                    size_t(need) * sizeof(float));
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadTubePoints: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_HasCudaMirror(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return _Impl(ctx)->model.HasCudaMirror() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

char const *
Tonic_GetDeviceFallbackReason(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return "";
        }
        return _Impl(ctx)->model.DeviceFallbackReason();
    } catch (...) {
        return "";
    }
}

int
Tonic_Undo(TonicModelContext *ctx, unsigned int *outDirty)
{
    try {
        if (!ctx) {
            _SetError("Tonic_Undo: null context");
            return TONIC_ERROR;
        }
        uint32_t dirty = 0;
        if (!_Impl(ctx)->model.Undo(&dirty)) {
            return TONIC_ERROR;
        }
        if (outDirty) {
            *outDirty = dirty;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Undo: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetUndoDepth(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return _Impl(ctx)->model.GetUndoDepth();
    } catch (...) {
        return 0;
    }
}

unsigned long long
Tonic_GetUndoBytes(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return _Impl(ctx)->model.GetUndoBytes();
    } catch (...) {
        return 0;
    }
}

int
Tonic_SetUndoBudget(TonicModelContext *ctx, int maxDepth,
                    unsigned long long maxBytes)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetUndoBudget: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetUndoBudget(
                maxDepth, uint64_t(maxBytes))) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetUndoBudget: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ClearUndo(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ClearUndo: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.ClearUndo();
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ClearUndo: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterCreate(TonicModelContext *modelCtx, const char *groomPath,
                      const char *descPath, TonicCommitterContext **outCc)
{
    try {
        if (!modelCtx || !outCc) {
            _SetError("Tonic_CommitterCreate: null context or out-param");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicCommitPaths paths;
        if (groomPath && *groomPath) {
            paths.groomPath = SdfPath(groomPath);
        }
        if (descPath && *descPath) {
            paths.descriptionPath = SdfPath(descPath);
        }
        if (paths.groomPath.IsEmpty() ||
            !paths.groomPath.IsAbsolutePath()) {
            _SetError("Tonic_CommitterCreate: groom path must be absolute");
            return TONIC_ERROR;
        }
        std::unique_ptr<TonicCommitterContextImpl> impl(
            new (std::nothrow) TonicCommitterContextImpl());
        if (!impl) {
            _SetError("Tonic_CommitterCreate: allocation failed");
            return TONIC_ERROR;
        }
        impl->committer.reset(new usdGenTonic::TonicCommitter(
            &_Impl(modelCtx)->model, paths));
        *outCc = reinterpret_cast<TonicCommitterContext *>(impl.release());
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterCreate: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterDestroy(TonicCommitterContext *cc)
{
    try {
        delete _CImpl(cc);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterDestroy: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterEnqueue(TonicCommitterContext *cc, int createOp, int setGuides,
                       int setRegion, const char *opPath)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterEnqueue: null committer");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicFillPlan plan;
        plan.createInterpOp = createOp != 0;
        plan.setInterpGuides = setGuides != 0;
        plan.setInterpRegion = setRegion != 0;
        if (opPath && *opPath) {
            plan.opPath = SdfPath(opPath);
        }
        _CImpl(cc)->committer->EnqueuePlan(plan);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterEnqueue: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterSwap(TonicCommitterContext *cc, const char *liveIdentifier,
                    int gestureActive)
{
    try {
        if (!cc || !liveIdentifier) {
            _SetError("Tonic_CommitterSwap: null argument");
            return TONIC_ERROR;
        }
        SdfLayerHandle live =
            SdfLayer::Find(std::string(liveIdentifier));
        if (!live) {
            _SetError("Tonic_CommitterSwap: unknown live layer");
            return TONIC_ERROR;
        }
        return int(_CImpl(cc)->committer->SwapIfIdle(
            live, gestureActive != 0));
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterSwap: unknown exception");
        return TONIC_ERROR;
    }
}

unsigned long long
Tonic_CommitterCommittedVersion(TonicCommitterContext const *cc)
{
    try {
        if (!cc) {
            return 0;
        }
        return _CImpl(cc)->committer->CommittedVersion();
    } catch (...) {
        return 0;
    }
}

unsigned long long
Tonic_CommitterPendingVersion(TonicCommitterContext const *cc)
{
    try {
        if (!cc) {
            return 0;
        }
        return _CImpl(cc)->committer->PendingVersion();
    } catch (...) {
        return 0;
    }
}

double
Tonic_CommitterLastSwapMs(TonicCommitterContext const *cc)
{
    try {
        if (!cc) {
            return 0.0;
        }
        return _CImpl(cc)->committer->LastSwapMs();
    } catch (...) {
        return 0.0;
    }
}

int
Tonic_CommitterPartialMode(TonicCommitterContext const *cc)
{
    try {
        if (!cc) {
            return 0;
        }
        return _CImpl(cc)->committer->PartialMode() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_CommitterCancelCooks(TonicCommitterContext *cc)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterCancelCooks: null committer");
            return -1;
        }
        return int(_CImpl(cc)->committer->CancelDescriptionCooks());
    } catch (std::exception const &e) {
        _SetError(e.what());
        return -1;
    } catch (...) {
        _SetError("Tonic_CommitterCancelCooks: unknown exception");
        return -1;
    }
}

int
Tonic_CommitterSetSwapBudgetMs(TonicCommitterContext *cc, double ms)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterSetSwapBudgetMs: null committer");
            return TONIC_ERROR;
        }
        _CImpl(cc)->committer->SetSwapBudgetMs(ms);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterSetSwapBudgetMs: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterDetach(TonicCommitterContext *cc)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterDetach: null committer");
            return TONIC_ERROR;
        }
        _CImpl(cc)->committer->Detach();
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterDetach: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterReattach(TonicCommitterContext *cc)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterReattach: null committer");
            return TONIC_ERROR;
        }
        _CImpl(cc)->committer->Reattach();
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterReattach: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BindScalp(TonicModelContext *ctx, float const *points, int pointFloats,
                int const *faceVertexCounts, int faceCount,
                int const *faceVertexIndices, int indexCount)
{
    try {
        if (!ctx || !points || !faceVertexCounts || !faceVertexIndices ||
            pointFloats <= 0 || faceCount <= 0 || indexCount <= 0) {
            _SetError("Tonic_BindScalp: null or empty scalp input");
            return TONIC_ERROR;
        }
        std::vector<float> pts(points, points + pointFloats);
        std::vector<int> counts(faceVertexCounts,
                                faceVertexCounts + faceCount);
        std::vector<int> indices(faceVertexIndices,
                                 faceVertexIndices + indexCount);
        if (!_Impl(ctx)->model.BindScalp(pts, counts, indices)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BindScalp: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_HasScalp(TonicModelContext const *ctx)
{
    try {
        return (ctx && _Impl(ctx)->model.HasScalp()) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_Raycast(TonicModelContext *ctx, float const origin[3],
              float const dir[3], int *outHit, int *outFace, float outUV[2],
              float outP[3], float outN[3])
{
    try {
        if (!ctx || !origin || !dir) {
            _SetError("Tonic_Raycast: null argument");
            return TONIC_ERROR;
        }
        _WriteHit(_Impl(ctx)->model.Raycast(origin, dir), outHit, outFace,
                  outUV, outP, outN);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Raycast: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ClosestPoint(TonicModelContext *ctx, float const p[3], int *outHit,
                   int *outFace, float outUV[2], float outP[3], float outN[3])
{
    try {
        if (!ctx || !p) {
            _SetError("Tonic_ClosestPoint: null argument");
            return TONIC_ERROR;
        }
        _WriteHit(_Impl(ctx)->model.ClosestPoint(p), outHit, outFace, outUV,
                  outP, outN);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ClosestPoint: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphAddNode(TonicModelContext *ctx, int faceId, float u, float v,
                   int *outId)
{
    try {
        if (!ctx || !outId) {
            _SetError("Tonic_GraphAddNode: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicHit hit;
        if (!_Locate(_Impl(ctx)->model, faceId, u, v, &hit)) {
            _SetError("Tonic_GraphAddNode: (face, uv) is off the scalp");
            return TONIC_ERROR;
        }
        int const id = _Impl(ctx)->model.GraphAddNode(hit);
        if (id < 0) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        *outId = id;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphAddNode: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphCreateRegion(TonicModelContext *ctx, int const *nodeIds,
                        int const *faceIds, float const *uvs, int count,
                        int *outRegionId)
{
    try {
        if (!ctx || !nodeIds || !faceIds || !uvs || count < 3) {
            _SetError("Tonic_GraphCreateRegion: invalid argument");
            return TONIC_ERROR;
        }
        std::vector<int> ids(nodeIds, nodeIds + count);
        std::vector<usdGenTonic::TonicHit> hits(
            size_t(count), usdGenTonic::TonicHit{});
        for (int i = 0; i < count; ++i) {
            if (ids[size_t(i)] >= 0) {
                continue;  // Exact stable id: its supplied (face, uv) is ignored.
            }
            if (ids[size_t(i)] != -1 ||
                !_Locate(_Impl(ctx)->model, faceIds[i], uvs[i * 2 + 0],
                         uvs[i * 2 + 1], &hits[size_t(i)])) {
                _SetError("Tonic_GraphCreateRegion: (face, uv) is off the scalp");
                return TONIC_ERROR;
            }
        }
        int const regionId = _Impl(ctx)->model.GraphCreateRegion(ids, hits);
        if (regionId < 0) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (outRegionId) {
            *outRegionId = regionId;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphCreateRegion: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphMoveNode(TonicModelContext *ctx, int nodeId, int faceId, float u,
                    float v)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphMoveNode: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicHit hit;
        if (!_Locate(_Impl(ctx)->model, faceId, u, v, &hit)) {
            _SetError("Tonic_GraphMoveNode: (face, uv) is off the scalp");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphMoveNode(nodeId, hit)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphMoveNode: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphMoveNodes(TonicModelContext *ctx, int const *nodeIds,
                     int const *faceIds, float const *uvs, int count)
{
    try {
        if (!ctx || !nodeIds || !faceIds || !uvs || count <= 0) {
            _SetError("Tonic_GraphMoveNodes: invalid argument");
            return TONIC_ERROR;
        }
        std::vector<int> ids(nodeIds, nodeIds + count);
        std::vector<usdGenTonic::TonicHit> hits(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (!_Locate(_Impl(ctx)->model, faceIds[i], uvs[i * 2 + 0],
                         uvs[i * 2 + 1], &hits[size_t(i)])) {
                _SetError("Tonic_GraphMoveNodes: (face, uv) is off the scalp");
                return TONIC_ERROR;
            }
        }
        if (!_Impl(ctx)->model.GraphMoveNodes(ids, hits)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphMoveNodes: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphConnect(TonicModelContext *ctx, int a, int b, int *outEdge)
{
    try {
        if (!ctx || !outEdge) {
            _SetError("Tonic_GraphConnect: null argument");
            return TONIC_ERROR;
        }
        int const id = _Impl(ctx)->model.GraphConnect(a, b);
        if (id < 0) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        *outEdge = id;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphConnect: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphSplitEdge(TonicModelContext *ctx, int edgeId, int faceId, float u,
                     float v, int *outNode)
{
    try {
        if (!ctx || !outNode) {
            _SetError("Tonic_GraphSplitEdge: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicHit hit;
        if (!_Locate(_Impl(ctx)->model, faceId, u, v, &hit)) {
            _SetError("Tonic_GraphSplitEdge: (face, uv) is off the scalp");
            return TONIC_ERROR;
        }
        int const id = _Impl(ctx)->model.GraphSplitEdge(edgeId, hit);
        if (id < 0) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        *outNode = id;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphSplitEdge: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphWeld(TonicModelContext *ctx, int keep, int drop)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphWeld: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphWeld(keep, drop)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphWeld: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphWeldAll(TonicModelContext *ctx, float radius, int *outWelds)
{
    try {
        if (!ctx || !outWelds) {
            _SetError("Tonic_GraphWeldAll: null argument");
            return TONIC_ERROR;
        }
        int const welds = _Impl(ctx)->model.GraphWeldAll(radius);
        if (welds < 0) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        *outWelds = welds;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphWeldAll: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphUnweld(TonicModelContext *ctx, int nodeId, int *outIds, int maxOut,
                  int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_GraphUnweld: null argument");
            return TONIC_ERROR;
        }
        std::vector<int> created = _Impl(ctx)->model.GraphUnweld(nodeId);
        *outCount = int(created.size());
        if (outIds) {
            for (size_t i = 0;
                 i < created.size() && int(i) < maxOut; ++i) {
                outIds[i] = created[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphUnweld: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphDeleteEdge(TonicModelContext *ctx, int edgeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphDeleteEdge: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphDeleteEdge(edgeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphDeleteEdge: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphDeleteNode(TonicModelContext *ctx, int nodeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphDeleteNode: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphDeleteNode(nodeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphDeleteNode: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphSnapNode(TonicModelContext const *ctx, float const p[3],
                    float radius)
{
    try {
        if (!ctx || !p) {
            return -1;
        }
        return _Impl(ctx)->model.GraphSnapNode(p, radius);
    } catch (...) {
        return -1;
    }
}

int
Tonic_GraphSnapEdge(TonicModelContext const *ctx, float const p[3],
                    float radius)
{
    try {
        if (!ctx || !p) {
            return -1;
        }
        return _Impl(ctx)->model.GraphSnapEdge(p, radius);
    } catch (...) {
        return -1;
    }
}

int
Tonic_GraphGetNode(TonicModelContext const *ctx, int nodeId,
                   int *outFaceId, float outUV[2], float outP[3])
{
    try {
        if (!ctx || !outFaceId || !outUV || !outP) {
            _SetError("Tonic_GraphGetNode: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicGraphNode node;
        if (!_Impl(ctx)->model.GraphGetNode(nodeId, &node)) {
            _SetError("Tonic_GraphGetNode: unknown node id");
            return TONIC_ERROR;
        }
        *outFaceId = node.faceId;
        outUV[0] = node.u;
        outUV[1] = node.v;
        outP[0] = node.p[0];
        outP[1] = node.p[1];
        outP[2] = node.p[2];
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphGetNode: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphGetEdge(TonicModelContext const *ctx, int edgeId,
                   int outNodeIds[2])
{
    try {
        if (!ctx || !outNodeIds) {
            _SetError("Tonic_GraphGetEdge: null argument");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphGetEdge(edgeId, outNodeIds)) {
            _SetError("Tonic_GraphGetEdge: unknown edge id");
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphGetEdge: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphGetNodeDisplayPosition(TonicModelContext const *ctx, int nodeId,
                                  float outP[3])
{
    try {
        if (!ctx || !outP) {
            _SetError("Tonic_GraphGetNodeDisplayPosition: null argument");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphGetNodeDisplayPosition(nodeId, outP)) {
            _SetError("Tonic_GraphGetNodeDisplayPosition: unknown node id");
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphGetNodeDisplayPosition: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphLinkRegions(TonicModelContext *ctx, int r0, int r1)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphLinkRegions: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphLinkRegions(r0, r1)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphLinkRegions: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphUnlinkRegions(TonicModelContext *ctx, int r0, int r1)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GraphUnlinkRegions: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GraphUnlinkRegions(r0, r1)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphUnlinkRegions: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphStroke(TonicModelContext *ctx, int const *faceIds,
                  float const *uvs, int samples, float snapRadius,
                  float simplifyEps, int *outNodes, int maxOut, int *outCount,
                  int *outClosed, int *outWeldedStart, int *outWeldedEnd)
{
    try {
        if (!ctx || !faceIds || !uvs || samples < 0 || !outCount) {
            _SetError("Tonic_GraphStroke: null argument");
            return TONIC_ERROR;
        }
        std::vector<usdGenTonic::TonicHit> hits;
        hits.reserve(size_t(samples));
        for (int i = 0; i < samples; ++i) {
            usdGenTonic::TonicHit hit;
            if (!_Locate(_Impl(ctx)->model, faceIds[i], uvs[i * 2],
                         uvs[i * 2 + 1], &hit)) {
                _SetError("Tonic_GraphStroke: a sample is off the scalp");
                return TONIC_ERROR;
            }
            hits.push_back(hit);
        }
        usdGenTonic::TonicStrokeResult result =
            _Impl(ctx)->model.GraphStroke(hits, snapRadius, simplifyEps);
        *outCount = int(result.nodeIds.size());
        if (outNodes) {
            for (size_t i = 0;
                 i < result.nodeIds.size() && int(i) < maxOut; ++i) {
                outNodes[i] = result.nodeIds[i];
            }
        }
        if (outClosed) {
            *outClosed = result.closedLoop ? 1 : 0;
        }
        if (outWeldedStart) {
            *outWeldedStart = result.weldedStart ? 1 : 0;
        }
        if (outWeldedEnd) {
            *outWeldedEnd = result.weldedEnd ? 1 : 0;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphStroke: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GraphMirrorX(TonicModelContext *ctx, int *outPairs, int maxPairs,
                   int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_GraphMirrorX: null argument");
            return TONIC_ERROR;
        }
        std::vector<std::pair<int, int>> mapping =
            _Impl(ctx)->model.GraphMirrorX();
        *outCount = int(mapping.size());
        if (outPairs) {
            for (size_t i = 0;
                 i < mapping.size() && int(i) < maxPairs; ++i) {
                outPairs[i * 2 + 0] = mapping[i].first;
                outPairs[i * 2 + 1] = mapping[i].second;
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GraphMirrorX: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetSnapRadius(TonicModelContext *ctx, float radius)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetSnapRadius: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetSnapRadius(radius);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetSnapRadius: unknown exception");
        return TONIC_ERROR;
    }
}

float
Tonic_GetSnapRadius(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetSnapRadius() : 0.0f;
    } catch (...) {
        return 0.0f;
    }
}

int
Tonic_SetMirrorX(TonicModelContext *ctx, int on)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetMirrorX: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetMirrorX(on != 0);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetMirrorX: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetMirrorX(TonicModelContext const *ctx)
{
    try {
        return (ctx && _Impl(ctx)->model.GetMirrorX()) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_Rasterise(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_Rasterise: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.Rasterise()) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Rasterise: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_RegionAtSurface(TonicModelContext const *ctx, int faceId, float u,
                      float v)
{
    try {
        return ctx ? _Impl(ctx)->model.RegionAtSurface(faceId, u, v) : -1;
    } catch (...) {
        return -1;
    }
}

unsigned long long
Tonic_GetMapVersion(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetMapVersion() : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetGraphCounts(TonicModelContext const *ctx, int *outNodes,
                     int *outEdges, int *outRegions)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetGraphCounts: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        if (outNodes) {
            *outNodes = int(snap.nodes.size());
        }
        if (outEdges) {
            *outEdges = int(snap.edges.size());
        }
        if (outRegions) {
            *outRegions = int(snap.regionLoops.size());
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetGraphCounts: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetRegionStats(TonicModelContext const *ctx, int *outRegions,
                     int *outUncovered, int *outIntersected)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetRegionStats: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        if (outRegions) {
            *outRegions = int(snap.regionLoops.size());
        }
        if (outUncovered) {
            *outUncovered = snap.uncoveredCount;
        }
        if (outIntersected) {
            *outIntersected = snap.intersectedCount;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetRegionStats: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadFaceRegions(TonicModelContext *ctx, int *out, int maxOut,
                      int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadFaceRegions: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        *outCount = int(snap.faceRegions.size());
        if (out) {
            if (maxOut < int(snap.faceRegions.size())) {
                _SetError("Tonic_ReadFaceRegions: output too small");
                return TONIC_ERROR;
            }
            std::memcpy(out, snap.faceRegions.data(),
                        snap.faceRegions.size() * sizeof(int));
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadFaceRegions: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadFaceRegionIds(TonicModelContext *ctx, int *out, int maxOut,
                        int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadFaceRegionIds: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        *outCount = int(snap.faceRegionIds.size());
        if (out) {
            if (maxOut < int(snap.faceRegionIds.size())) {
                _SetError("Tonic_ReadFaceRegionIds: output too small");
                return TONIC_ERROR;
            }
            std::memcpy(out, snap.faceRegionIds.data(),
                        snap.faceRegionIds.size() * sizeof(int));
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadFaceRegionIds: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadGraphNodes(TonicModelContext *ctx, int *outFaceIds, float *outUV,
                     float *outP, int maxNodes, int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadGraphNodes: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        *outCount = int(snap.nodes.size());
        if ((outFaceIds || outUV || outP) &&
            maxNodes < int(snap.nodes.size())) {
            _SetError("Tonic_ReadGraphNodes: output too small");
            return TONIC_ERROR;
        }
        for (size_t i = 0; i < snap.nodes.size(); ++i) {
            if (outFaceIds) {
                outFaceIds[i] = snap.nodes[i].faceId;
            }
            if (outUV) {
                outUV[i * 2 + 0] = snap.nodes[i].u;
                outUV[i * 2 + 1] = snap.nodes[i].v;
            }
            if (outP) {
                outP[i * 3 + 0] = snap.nodes[i].p[0];
                outP[i * 3 + 1] = snap.nodes[i].p[1];
                outP[i * 3 + 2] = snap.nodes[i].p[2];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadGraphNodes: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadGraphEdges(TonicModelContext *ctx, int *outPairs, int maxEdges,
                     int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadGraphEdges: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        *outCount = int(snap.edges.size());
        if (outPairs) {
            if (maxEdges < int(snap.edges.size())) {
                _SetError("Tonic_ReadGraphEdges: output too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < snap.edges.size(); ++i) {
                outPairs[i * 2 + 0] = snap.edges[i].first;
                outPairs[i * 2 + 1] = snap.edges[i].second;
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadGraphEdges: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadRegionColors(TonicModelContext *ctx, float *outRGB, int maxRegions,
                       int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadRegionColors: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        int const regions = int(snap.regionColors.size() / 3);
        *outCount = regions;
        if (outRGB) {
            if (maxRegions < regions) {
                _SetError("Tonic_ReadRegionColors: output too small");
                return TONIC_ERROR;
            }
            std::memcpy(outRGB, snap.regionColors.data(),
                        snap.regionColors.size() * sizeof(float));
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadRegionColors: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadRegionLoops(TonicModelContext *ctx, int *outCounts, int maxRegions,
                      int *outIndices, int maxIndices, int *outRegionCount,
                      int *outIndexCount)
{
    try {
        if (!ctx || !outRegionCount || !outIndexCount) {
            _SetError("Tonic_ReadRegionLoops: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GraphSnapshot snap =
            _Impl(ctx)->model.SnapshotGraph();
        int total = 0;
        for (auto const &loop : snap.regionLoops) {
            total += int(loop.size());
        }
        *outRegionCount = int(snap.regionLoops.size());
        *outIndexCount = total;
        if (outCounts) {
            if (maxRegions < int(snap.regionLoops.size())) {
                _SetError("Tonic_ReadRegionLoops: counts too small");
                return TONIC_ERROR;
            }
            for (size_t r = 0; r < snap.regionLoops.size(); ++r) {
                outCounts[r] = int(snap.regionLoops[r].size());
            }
        }
        if (outIndices) {
            if (maxIndices < total) {
                _SetError("Tonic_ReadRegionLoops: indices too small");
                return TONIC_ERROR;
            }
            int o = 0;
            for (auto const &loop : snap.regionLoops) {
                for (int nid : loop) {
                    outIndices[o++] = nid;
                }
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadRegionLoops: unknown exception");
        return TONIC_ERROR;
    }
}

// -- P3 Tube + Fill modes + pick --------------------------------------------

int
Tonic_BuildTubeFromRegion(TonicModelContext *ctx, int regionId,
                          int centerCount, int ringVerts, float length)
{
    try {
        if (!ctx) {
            _SetError("Tonic_BuildTubeFromRegion: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.BuildTubeFromRegion(regionId, centerCount,
                                                   ringVerts, length)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BuildTubeFromRegion: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MoveCenterCV(TonicModelContext *ctx, int cv, float dx, float dy,
                   float dz)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MoveCenterCV: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MoveCenterCV(cv, dx, dy, dz)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MoveCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_InsertCenterCV(TonicModelContext *ctx, int atIndex)
{
    try {
        if (!ctx) {
            _SetError("Tonic_InsertCenterCV: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.InsertCenterCV(atIndex)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_InsertCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_DeleteCenterCV(TonicModelContext *ctx, int index)
{
    try {
        if (!ctx) {
            _SetError("Tonic_DeleteCenterCV: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.DeleteCenterCV(index)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_DeleteCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetTubeLength(TonicModelContext *ctx, float length)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetTubeLength: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetTubeLength(length)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetTubeLength: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MatchSurface(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MatchSurface: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MatchSurface()) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MatchSurface: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetCenterCVCount(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetCenterCVCount() : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_GetCenterCV(TonicModelContext *ctx, int cv, float outXYZ[3])
{
    try {
        if (!ctx || !outXYZ) {
            _SetError("Tonic_GetCenterCV: null argument");
            return TONIC_ERROR;
        }
        float x = 0, y = 0, z = 0;
        if (!_Impl(ctx)->model.GetCenterCV(cv, &x, &y, &z)) {
            _SetError("Tonic_GetCenterCV: CV out of range");
            return TONIC_ERROR;
        }
        outXYZ[0] = x;
        outXYZ[1] = y;
        outXYZ[2] = z;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetSectionCount(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetSectionCount() : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_GetSection(TonicModelContext *ctx, int ring, float *outT, float *outUV,
                 int uvLen, int *outCount, float *outScale, float *outTwist)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_GetSection: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeSection section;
        if (!_Impl(ctx)->model.GetSection(ring, &section)) {
            _SetError("Tonic_GetSection: ring out of range");
            return TONIC_ERROR;
        }
        *outCount = int(section.u.size());
        if (outT) {
            *outT = section.t;
        }
        if (outScale) {
            *outScale = section.scale;
        }
        if (outTwist) {
            *outTwist = section.twist;
        }
        if (outUV) {
            if (uvLen < int(section.u.size()) * 2) {
                _SetError("Tonic_GetSection: output too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < section.u.size(); ++i) {
                outUV[i * 2 + 0] = section.u[i];
                outUV[i * 2 + 1] = section.v[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetSection: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MoveSectionRing(TonicModelContext *ctx, int ring, float du, float dv)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MoveSectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MoveSectionRing(ring, du, dv)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MoveSectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ScaleSectionRing(TonicModelContext *ctx, int ring, float scale)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ScaleSectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.ScaleSectionRing(ring, scale)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ScaleSectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_TwistSectionRing(TonicModelContext *ctx, int ring, float radians)
{
    try {
        if (!ctx) {
            _SetError("Tonic_TwistSectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.TwistSectionRing(ring, radians)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_TwistSectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MoveSectionCV(TonicModelContext *ctx, int ring, int slot, float du,
                    float dv)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MoveSectionCV: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MoveSectionCV(ring, slot, du, dv)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MoveSectionCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_AddSectionRing(TonicModelContext *ctx, float t, int *outRing)
{
    try {
        if (!ctx) {
            _SetError("Tonic_AddSectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.AddSectionRing(t)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (outRing) {
            // Inserted rings sort by t; report the new ring count so the
            // caller re-reads census.
            *outRing = _Impl(ctx)->model.GetSectionCount();
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_AddSectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_RemoveSectionRing(TonicModelContext *ctx, int ring)
{
    try {
        if (!ctx) {
            _SetError("Tonic_RemoveSectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.RemoveSectionRing(ring)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_RemoveSectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CopySectionRing(TonicModelContext *ctx, int src, int dst)
{
    try {
        if (!ctx) {
            _SetError("Tonic_CopySectionRing: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.CopySectionRing(src, dst)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CopySectionRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetSoftSelection(TonicModelContext *ctx, float center, float radius)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetSoftSelection: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetSoftSelection(center, radius)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetSoftSelection: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetSoftSelection(TonicModelContext *ctx, float *outCenter,
                       float *outRadius)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetSoftSelection: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.GetSoftSelection(outCenter, outRadius);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetSoftSelection: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_RelaxCenter(TonicModelContext *ctx, float strength, int iterations)
{
    try {
        if (!ctx) {
            _SetError("Tonic_RelaxCenter: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.RelaxCenter(strength, iterations)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_RelaxCenter: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SnapRootToScalp(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SnapRootToScalp: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SnapRootToScalp()) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SnapRootToScalp: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetDisplaySegments(TonicModelContext *ctx, int segments)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetDisplaySegments: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetDisplaySegments(segments)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetDisplaySegments: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetDisplaySegments(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetDisplaySegments() : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_SetTubeRegionId(TonicModelContext *ctx, int regionId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetTubeRegionId: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetTubeRegionId(regionId);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetTubeRegionId: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeRegionId(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetTubeRegionId() : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_ReadTubeRegionFaces(TonicModelContext *ctx, int *out, int maxOut,
                          int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadTubeRegionFaces: null argument");
            return TONIC_ERROR;
        }
        std::vector<int> faces = _Impl(ctx)->model.TubeRegionFaces();
        *outCount = int(faces.size());
        if (out) {
            if (maxOut < int(faces.size())) {
                _SetError("Tonic_ReadTubeRegionFaces: output too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < faces.size(); ++i) {
                out[i] = faces[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadTubeRegionFaces: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetFillParams(TonicModelContext *ctx, float density, int cvCount,
                    int seed, float edgeBias, float const *profilePairs,
                    int pairFloats)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetFillParams: null context");
            return TONIC_ERROR;
        }
        if (pairFloats < 0 || pairFloats % 2 != 0 ||
            (pairFloats > 0 && !profilePairs)) {
            _SetError("Tonic_SetFillParams: profile holds pairs");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::FillParams params;
        params.density = density;
        params.cvCount = cvCount;
        params.seed = seed;
        params.edgeBias = edgeBias;
        params.lengthProfile.assign(
            profilePairs ? profilePairs : nullptr,
            profilePairs ? profilePairs + pairFloats : nullptr);
        if (!_Impl(ctx)->model.SetFillParams(std::move(params))) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetFillParams: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetFillParams(TonicModelContext *ctx, float *outDensity, int *outCvCount,
                    int *outSeed, float *outEdgeBias, float *outProfile,
                    int maxFloats, int *outFloats)
{
    try {
        if (!ctx || !outFloats) {
            _SetError("Tonic_GetFillParams: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::FillParams params =
            _Impl(ctx)->model.GetFillParams();
        if (outDensity) {
            *outDensity = params.density;
        }
        if (outCvCount) {
            *outCvCount = params.cvCount;
        }
        if (outSeed) {
            *outSeed = params.seed;
        }
        if (outEdgeBias) {
            *outEdgeBias = params.edgeBias;
        }
        *outFloats = int(params.lengthProfile.size());
        if (outProfile) {
            if (maxFloats < int(params.lengthProfile.size())) {
                _SetError("Tonic_GetFillParams: profile output too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < params.lengthProfile.size(); ++i) {
                outProfile[i] = params.lengthProfile[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetFillParams: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetOutputSettings(TonicModelContext *ctx, int enabled,
                        float densityMultiplier, float width)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetOutputSettings: null context");
            return TONIC_ERROR;
        }
        if (enabled != 0 && enabled != 1) {
            _SetError("Tonic_SetOutputSettings: enabled must be 0 or 1");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::OutputSettings settings =
            _Impl(ctx)->model.GetOutputSettings();
        settings.enabled = enabled != 0;
        settings.densityMultiplier = densityMultiplier;
        settings.width = width;
        if (!_Impl(ctx)->model.SetOutputSettings(settings)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetOutputSettings: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetOutputSettings(TonicModelContext const *ctx, int *outEnabled,
                        float *outDensityMultiplier, float *outWidth)
{
    try {
        if (!ctx || !outEnabled || !outDensityMultiplier || !outWidth) {
            _SetError("Tonic_GetOutputSettings: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::OutputSettings const settings =
            _Impl(ctx)->model.GetOutputSettings();
        *outEnabled = settings.enabled ? 1 : 0;
        *outDensityMultiplier = settings.densityMultiplier;
        *outWidth = settings.width;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetOutputSettings: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetPreviewFraction(TonicModelContext *ctx, float fraction)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetPreviewFraction: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetPreviewFraction(fraction)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetPreviewFraction: unknown exception");
        return TONIC_ERROR;
    }
}

float
Tonic_GetPreviewFraction(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetPreviewFraction() : 0.0f;
    } catch (...) {
        return 0.0f;
    }
}

int
Tonic_SetFreezeRoots(TonicModelContext *ctx, int on)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetFreezeRoots: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetFreezeRoots(on != 0);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetFreezeRoots: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetFreezeRoots(TonicModelContext const *ctx)
{
    try {
        return ctx && _Impl(ctx)->model.GetFreezeRoots() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_RefillGuides(TonicModelContext *ctx, float fraction)
{
    try {
        if (!ctx) {
            _SetError("Tonic_RefillGuides: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.RefillGuides(fraction)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_RefillGuides: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GenerateGuides(TonicModelContext *ctx, float fraction)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GenerateGuides: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.GenerateGuides(fraction)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    }
}

int
Tonic_ClearGeneratedCurves(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ClearGeneratedCurves: null context");
            return TONIC_ERROR;
        }
        return _Impl(ctx)->model.ClearGeneratedCurves() ? TONIC_OK
                                                         : TONIC_ERROR;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    }
}

int
Tonic_SetGeneratedCurvesVisible(TonicModelContext *ctx, int visible)
{
    if (!ctx) {
        _SetError("Tonic_SetGeneratedCurvesVisible: null context");
        return TONIC_ERROR;
    }
    return _Impl(ctx)->model.SetGeneratedCurvesVisible(visible != 0)
               ? TONIC_OK
               : TONIC_ERROR;
}

int
Tonic_GetGeneratedCurvesVisible(TonicModelContext const *ctx, int *outVisible)
{
    if (!ctx || !outVisible) {
        _SetError("Tonic_GetGeneratedCurvesVisible: null argument");
        return TONIC_ERROR;
    }
    *outVisible = _Impl(ctx)->model.GetGeneratedCurvesVisible() ? 1 : 0;
    return TONIC_OK;
}

int
Tonic_GetGuideCounts(TonicModelContext const *ctx, int *outGuides, int *outCv)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetGuideCounts: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GuidePreview preview =
            _Impl(ctx)->model.GetGuidePreview();
        if (outGuides) {
            *outGuides = preview.guideCount;
        }
        if (outCv) {
            *outCv = preview.cvCount;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetGuideCounts: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadGuidePreview(TonicModelContext *ctx, float *outXYZ, int xyzLen,
                       int *outCounts, int countsLen, int *outGuideCount)
{
    try {
        if (!ctx || !outGuideCount) {
            _SetError("Tonic_ReadGuidePreview: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::GuidePreview preview =
            _Impl(ctx)->model.GetGuidePreview();
        *outGuideCount = preview.guideCount;
        if (outXYZ) {
            if (xyzLen < int(preview.points.size())) {
                _SetError("Tonic_ReadGuidePreview: points too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < preview.points.size(); ++i) {
                outXYZ[i] = preview.points[i];
            }
        }
        if (outCounts) {
            if (countsLen < int(preview.counts.size())) {
                _SetError("Tonic_ReadGuidePreview: counts too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < preview.counts.size(); ++i) {
                outCounts[i] = preview.counts[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadGuidePreview: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadGuideRoots(TonicModelContext *ctx, int *outFaceIds, float *outXYZ,
                     float *outRU, int maxRoots, int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadGuideRoots: null argument");
            return TONIC_ERROR;
        }
        // Snapshot the roots under the model's mutex (GetRoots is
        // UI-thread-only by convention; the vector copy is the snapshot).
        std::vector<usdGenTonic::TonicGuideRoot> roots =
            _Impl(ctx)->model.GetRoots();
        *outCount = int(roots.size());
        if ((outFaceIds || outXYZ || outRU) && maxRoots < int(roots.size())) {
            _SetError("Tonic_ReadGuideRoots: output too small");
            return TONIC_ERROR;
        }
        for (size_t i = 0; i < roots.size(); ++i) {
            if (outFaceIds) {
                outFaceIds[i] = roots[i].faceId;
            }
            if (outXYZ) {
                outXYZ[i * 3 + 0] = roots[i].px;
                outXYZ[i * 3 + 1] = roots[i].py;
                outXYZ[i * 3 + 2] = roots[i].pz;
            }
            if (outRU) {
                outRU[i * 2 + 0] = roots[i].ru;
                outRU[i * 2 + 1] = roots[i].rv;
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadGuideRoots: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_Pick(TonicModelContext *ctx, float const viewProj[16], int w, int h,
           float x, float y, float radiusPx, unsigned int kindMask,
           int *outHit, unsigned int *outKind, int *outIndex, int *outSubIndex,
           float *outDistPx, float *outDepth)
{
    try {
        if (!ctx || !viewProj || !outHit) {
            _SetError("Tonic_Pick: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicPickHit hit = _Impl(ctx)->model.Pick(
            viewProj, w, h, x, y, radiusPx, uint32_t(kindMask));
        *outHit = hit.hit ? 1 : 0;
        if (!hit.hit) {
            return TONIC_OK;
        }
        if (outKind) {
            *outKind = hit.kind;
        }
        if (outIndex) {
            *outIndex = hit.index;
        }
        if (outSubIndex) {
            *outSubIndex = hit.subIndex;
        }
        if (outDistPx) {
            *outDistPx = hit.distPx;
        }
        if (outDepth) {
            *outDepth = hit.depth;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Pick: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CommitterSetScalpPath(TonicCommitterContext *cc, const char *scalpPath)
{
    try {
        if (!cc) {
            _SetError("Tonic_CommitterSetScalpPath: null committer");
            return TONIC_ERROR;
        }
        if (!scalpPath || !*scalpPath) {
            _CImpl(cc)->committer->SetScalpPath(SdfPath());
        } else {
            SdfPath const path(scalpPath);
            if (!path.IsAbsolutePath()) {
                _SetError("Tonic_CommitterSetScalpPath: path must be absolute");
                return TONIC_ERROR;
            }
            _CImpl(cc)->committer->SetScalpPath(path);
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CommitterSetScalpPath: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BakeCreate(TonicModelContext *modelCtx, const char *outDir,
                 const char *baseName, TonicBakeContext **outBake)
{
    try {
        if (!modelCtx || !outDir || !*outDir || !outBake) {
            _SetError("Tonic_BakeCreate: null argument");
            return TONIC_ERROR;
        }
        std::unique_ptr<TonicBakeContextImpl> impl(
            new (std::nothrow) TonicBakeContextImpl());
        if (!impl) {
            _SetError("Tonic_BakeCreate: allocation failed");
            return TONIC_ERROR;
        }
        impl->model = &_Impl(modelCtx)->model;
        impl->worker.reset(new usdGenTonic::TonicBakeWorker());
        impl->outDir = outDir;
        if (baseName && *baseName) {
            impl->baseName = baseName;
        }
        *outBake = reinterpret_cast<TonicBakeContext *>(impl.release());
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeCreate: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BakeDestroy(TonicBakeContext *bake)
{
    try {
        delete _BImpl(bake);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeDestroy: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BakeSetOptions(TonicBakeContext *bake, int resOverride, int levelCount)
{
    try {
        if (!bake) {
            _SetError("Tonic_BakeSetOptions: null bake context");
            return TONIC_ERROR;
        }
        if (levelCount < 1) {
            _SetError("Tonic_BakeSetOptions: levelCount must be >= 1");
            return TONIC_ERROR;
        }
        if (!_BImpl(bake)->model->SetOutputPtexResolution(resOverride)) {
            _SetError(_BImpl(bake)->model->GetDiagnostic());
            return TONIC_ERROR;
        }
        _BImpl(bake)->resOverride = resOverride;
        _BImpl(bake)->levelCount = levelCount;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeSetOptions: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BakeEnqueue(TonicBakeContext *bake)
{
    try {
        if (!bake) {
            _SetError("Tonic_BakeEnqueue: null bake context");
            return TONIC_ERROR;
        }
        TonicBakeContextImpl *impl = _BImpl(bake);
        if (!impl->model->HasScalp()) {
            _SetError("Tonic_BakeEnqueue: no scalp bound");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicBakeInput input;
        input.scalp = impl->model->GetScalp();
        input.graph = impl->model->GetGraph();
        input.levelCount = impl->levelCount;
        input.resOverride = impl->resOverride;
        input.outDir = impl->outDir;
        input.baseName = impl->baseName;
        impl->worker->Enqueue(impl->model->GetMapVersion(), std::move(input));
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeEnqueue: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BakeTakeCompleted(TonicBakeContext *bake, unsigned long long *outVersion,
                        char *outPath, int pathLen)
{
    try {
        if (!bake) {
            return 0;
        }
        uint64_t version = 0;
        std::string path;
        if (!_BImpl(bake)->worker->TakeCompleted(&version, &path)) {
            return 0;
        }
        if (outVersion) {
            *outVersion = version;
        }
        if (outPath && pathLen > 0) {
            std::strncpy(outPath, path.c_str(), size_t(pathLen) - 1);
            outPath[pathLen - 1] = '\0';
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

int
Tonic_BakeSwap(TonicBakeContext *bake, unsigned long long mapVersion,
               const char *liveIdentifier, const char *regionMapPath,
               const char *file)
{
    try {
        if (!bake || !liveIdentifier || !regionMapPath || !file) {
            _SetError("Tonic_BakeSwap: null argument");
            return TONIC_ERROR;
        }
        SdfLayerHandle live = SdfLayer::Find(std::string(liveIdentifier));
        if (!live) {
            _SetError("Tonic_BakeSwap: unknown live layer");
            return TONIC_ERROR;
        }
        std::string err;
        if (!usdGenTonic::TonicBakeSwapMapFile(
                live, SdfPath(regionMapPath), file, &err)) {
            _SetError(err.c_str());
            return TONIC_ERROR;
        }
        _BImpl(bake)->model->NoteBakedMapFile(uint64_t(mapVersion), file);
        _BImpl(bake)->worker->NoteSwapped(uint64_t(mapVersion), file);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeSwap: unknown exception");
        return TONIC_ERROR;
    }
}

unsigned long long
Tonic_BakePendingVersion(TonicBakeContext const *bake)
{
    try {
        return bake ? _BImpl(bake)->worker->PendingVersion() : 0;
    } catch (...) {
        return 0;
    }
}

unsigned long long
Tonic_BakeCompletedVersion(TonicBakeContext const *bake)
{
    try {
        return bake ? _BImpl(bake)->worker->CompletedVersion() : 0;
    } catch (...) {
        return 0;
    }
}

const char *
Tonic_GetLastError(void)
{
    return _tlError.c_str();
}

// -- P4: hierarchy + sculpt -----------------------------------------------

int
Tonic_SubdivideTube(TonicModelContext *ctx, int tubeId, int count,
                    const char *splitMode, int seed, int *outIds, int outCap,
                    int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_SubdivideTube: null context or count");
            return TONIC_ERROR;
        }
        std::vector<int> kids;
        if (!_Impl(ctx)->model.SubdivideTube(tubeId, count, splitMode, seed,
                                             &kids)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        *outCount = int(kids.size());
        if (outIds && outCap >= *outCount) {
            for (size_t i = 0; i < kids.size(); ++i) {
                outIds[i] = kids[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SubdivideTube: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MergeChildren(TonicModelContext *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MergeChildren: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MergeChildren(tubeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MergeChildren: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MergeSelected(TonicModelContext *ctx, int const *tubeIds, int idCount,
                    int *outKept)
{
    try {
        if (!ctx || !outKept) {
            _SetError("Tonic_MergeSelected: null context or output");
            return TONIC_ERROR;
        }
        if (!tubeIds || idCount <= 0) {
            _SetError("Tonic_MergeSelected: want tube ids");
            return TONIC_ERROR;
        }
        auto ids = std::vector<int>(size_t(idCount), 0);
        for (int i = 0; i < idCount; ++i) {
            ids[size_t(i)] = tubeIds[i];
        }
        if (!_Impl(ctx)->model.MergeSelected(ids, outKept)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MergeSelected: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeCount(TonicModelContext const *ctx)
{
    try {
        if (!ctx) {
            return 0;
        }
        return _Impl(ctx)->model.GetTubeCount();
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetTubeLevel(TonicModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            return -1;
        }
        return _Impl(ctx)->model.GetTubeLevel(tubeId);
    } catch (...) {
        return -1;
    }
}

int
Tonic_GetTubeChildren(TonicModelContext const *ctx, int tubeId, int *outIds,
                      int outCap, int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_GetTubeChildren: null context or count");
            return TONIC_ERROR;
        }
        std::vector<int> kids = _Impl(ctx)->model.GetTubeChildren(tubeId);
        *outCount = int(kids.size());
        if (outIds && outCap >= *outCount) {
            for (size_t i = 0; i < kids.size(); ++i) {
                outIds[i] = kids[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeChildren: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeCenterCount(TonicModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            return -1;
        }
        return _Impl(ctx)->model.GetTubeCenterCount(tubeId);
    } catch (...) {
        return -1;
    }
}

int
Tonic_GetTubeCenterCV(TonicModelContext const *ctx, int tubeId, int cv,
                      float *outXYZ)
{
    try {
        if (!ctx || !outXYZ) {
            _SetError("Tonic_GetTubeCenterCV: null context or output");
            return TONIC_ERROR;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!_Impl(ctx)->model.GetTubeCenterCV(tubeId, cv, &x, &y, &z)) {
            _SetError("Tonic_GetTubeCenterCV: unknown tube or CV");
            return TONIC_ERROR;
        }
        outXYZ[0] = x;
        outXYZ[1] = y;
        outXYZ[2] = z;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeCenterHandle(TonicModelContext const *ctx, int tubeId, int cv,
                          float *outXYZ)
{
    try {
        if (!ctx || !outXYZ) {
            _SetError("Tonic_GetTubeCenterHandle: null context or output");
            return TONIC_ERROR;
        }
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!_Impl(ctx)->model.GetTubeCenterHandle(tubeId, cv, &x, &y, &z)) {
            _SetError("Tonic_GetTubeCenterHandle: unknown tube, CV or "
                      "malformed section");
            return TONIC_ERROR;
        }
        outXYZ[0] = x;
        outXYZ[1] = y;
        outXYZ[2] = z;
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeCenterHandle: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MoveTubeCenterCV(TonicModelContext *ctx, int tubeId, int cv, float dx,
                       float dy, float dz)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MoveTubeCenterCV: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MoveTubeCenterCV(tubeId, cv, dx, dy, dz)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MoveTubeCenterCV: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_TranslateTube(TonicModelContext *ctx, int tubeId, float dx, float dy,
                    float dz)
{
    try {
        if (!ctx) {
            _SetError("Tonic_TranslateTube: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.TranslateTube(tubeId, dx, dy, dz)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_TranslateTube: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadTubeDeltas(TonicModelContext const *ctx, int tubeId, float *out,
                     int outCap, int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadTubeDeltas: null context or count");
            return TONIC_ERROR;
        }
        std::vector<float> flat = _Impl(ctx)->model.ReadTubeDeltas(tubeId);
        *outCount = int(flat.size());
        if (out && outCap >= *outCount) {
            for (size_t i = 0; i < flat.size(); ++i) {
                out[i] = flat[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadTubeDeltas: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetLockParents(TonicModelContext *ctx, int tubeId, int on)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetLockParents: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetTubeLockParents(tubeId, on != 0);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetLockParents: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetLockChildren(TonicModelContext *ctx, int tubeId, int on)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetLockChildren: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SetTubeLockChildren(tubeId, on != 0);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetLockChildren: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GroupTubes(TonicModelContext *ctx, int const *tubeIds, int idCount,
                 int makeTransient, int *outParent)
{
    try {
        if (!ctx || !outParent) {
            _SetError("Tonic_GroupTubes: null context or output");
            return TONIC_ERROR;
        }
        if (!tubeIds || idCount <= 0) {
            _SetError("Tonic_GroupTubes: want tube ids");
            return TONIC_ERROR;
        }
        auto ids = std::vector<int>(size_t(idCount), 0);
        for (int i = 0; i < idCount; ++i) {
            ids[size_t(i)] = tubeIds[i];
        }
        if (!_Impl(ctx)->model.GroupTubes(ids, makeTransient != 0,
                                          outParent)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GroupTubes: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_MakePersistent(TonicModelContext *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_MakePersistent: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.MakeTubePersistent(tubeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_MakePersistent: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SculptStroke(TonicModelContext *ctx, int tubeId, const char *brush,
                   int const *cvIds, float const *deltas, int cvCount,
                   int preserveLength, int mirrorX)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SculptStroke: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SculptStroke(tubeId, brush, cvIds, deltas,
                                            cvCount, preserveLength != 0,
                                            mirrorX != 0)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SculptStroke: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadSmoothnessScores(TonicModelContext const *ctx, float *out,
                           int outCap, int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadSmoothnessScores: null context or count");
            return TONIC_ERROR;
        }
        std::vector<float> scores = _Impl(ctx)->model.SmoothnessScores();
        *outCount = int(scores.size());
        if (out && outCap >= *outCount) {
            for (size_t i = 0; i < scores.size(); ++i) {
                out[i] = scores[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadSmoothnessScores: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CheckRootIntersections(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_CheckRootIntersections: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.CheckRootIntersections()) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CheckRootIntersections: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadIntersectedTubes(TonicModelContext const *ctx, int *out, int outCap,
                           int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadIntersectedTubes: null context or count");
            return TONIC_ERROR;
        }
        std::vector<int> ids = _Impl(ctx)->model.IntersectedTubes();
        *outCount = int(ids.size());
        if (out && outCap >= *outCount) {
            for (size_t i = 0; i < ids.size(); ++i) {
                out[i] = ids[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadIntersectedTubes: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadTubeIds(TonicModelContext const *ctx, int *out, int outCap,
                  int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadTubeIds: null context or count");
            return TONIC_ERROR;
        }
        std::vector<int> ids = _Impl(ctx)->model.TubeIds();
        *outCount = int(ids.size());
        if (out && outCap >= *outCount) {
            for (size_t i = 0; i < ids.size(); ++i) {
                out[i] = ids[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadTubeIds: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadL1TubeIds(TonicModelContext const *ctx, int *out, int outCap,
                    int *outCount)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_ReadL1TubeIds: null context or count");
            return TONIC_ERROR;
        }
        std::vector<int> ids = _Impl(ctx)->model.L1TubeIds();
        *outCount = int(ids.size());
        if (out && outCap >= *outCount) {
            for (size_t i = 0; i < ids.size(); ++i) {
                out[i] = ids[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadL1TubeIds: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_TubeForRegion(TonicModelContext const *ctx, int regionId)
{
    try {
        return ctx ? _Impl(ctx)->model.TubeForRegion(regionId) : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_RegionForTube(TonicModelContext const *ctx, int tubeId)
{
    try {
        return ctx ? _Impl(ctx)->model.RegionForTube(tubeId) : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_SyncRegionTubes(TonicModelContext *ctx, int *outRebuilt,
                      int *outRemoved)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SyncRegionTubes: null context");
            return TONIC_ERROR;
        }
        std::vector<int> rebuilt, removed;
        if (!_Impl(ctx)->model.SyncRegionTubes(&rebuilt, &removed)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (outRebuilt) {
            *outRebuilt = int(rebuilt.size());
        }
        if (outRemoved) {
            *outRemoved = int(removed.size());
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SyncRegionTubes: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ImportLockedTube(TonicModelContext *ctx, int parentId, float const *cx,
                       float const *cy, float const *cz, int nCv,
                       float const *secT, float const *secU, float const *secV,
                       int nSec, int ringVerts, int *outTubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ImportLockedTube: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.ImportLockedTube(
                parentId, cx, cy, cz, nCv, secT, secU, secV, nSec, ringVerts,
                outTubeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ImportLockedTube: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ImportSweptMesh(TonicModelContext *ctx, int parentId,
                      float const *points, int ringCount, int ringVerts,
                      int *outTubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ImportSweptMesh: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.ImportSweptMesh(parentId, points, ringCount,
                                               ringVerts, outTubeId)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ImportSweptMesh: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeSectionCount(TonicModelContext const *ctx, int tubeId)
{
    try {
        return ctx ? _Impl(ctx)->model.GetTubeSectionCount(tubeId) : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_GetTubeSection(TonicModelContext *ctx, int tubeId, int ring, float *outT,
                     float *outUV, int uvLen, int *outCount, float *outScale,
                     float *outTwist)
{
    try {
        if (!ctx || !outCount) {
            _SetError("Tonic_GetTubeSection: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeSection section;
        if (!_Impl(ctx)->model.GetTubeSection(tubeId, ring, &section)) {
            _SetError("Tonic_GetTubeSection: unknown tube or ring");
            return TONIC_ERROR;
        }
        *outCount = int(section.u.size());
        if (outT) {
            *outT = section.t;
        }
        if (outScale) {
            *outScale = section.scale;
        }
        if (outTwist) {
            *outTwist = section.twist;
        }
        if (outUV) {
            if (uvLen < int(section.u.size()) * 2) {
                _SetError("Tonic_GetTubeSection: output too small");
                return TONIC_ERROR;
            }
            for (size_t i = 0; i < section.u.size(); ++i) {
                outUV[i * 2 + 0] = section.u[i];
                outUV[i * 2 + 1] = section.v[i];
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeSection: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_IsTubeImported(TonicModelContext const *ctx, int tubeId)
{
    try {
        return ctx && _Impl(ctx)->model.IsTubeImported(tubeId) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

/* -- V0 viewport publication (plan/18 §2.1, §2.2) --------------------------- */

int
Tonic_GetModelId(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->modelId : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_Activate(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_Activate: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicRegistry &registry =
            usdGenTonic::TonicRegistry::Get();
        if (!registry.SetActive(_Impl(ctx)->modelId)) {
            _SetError("Tonic_Activate: model is not registered");
            return TONIC_ERROR;
        }
        /* Publish everything: this is where the test tube goes away and
         * the model's own levels appear. */
        registry.Publish(~0u);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Activate: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_Deactivate(TonicModelContext *ctx)
{
    try {
        (void)ctx;
        usdGenTonic::TonicRegistry &registry =
            usdGenTonic::TonicRegistry::Get();
        registry.SetActive(0);
        registry.Publish(~0u);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Deactivate: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_Publish(TonicModelContext *ctx, unsigned int dirtyMask)
{
    try {
        if (!ctx) {
            _SetError("Tonic_Publish: null context");
            return -1;
        }
        return usdGenTonic::TonicRegistry::Get().Publish(
            uint32_t(dirtyMask));
    } catch (std::exception const &e) {
        _SetError(e.what());
        return -1;
    } catch (...) {
        _SetError("Tonic_Publish: unknown exception");
        return -1;
    }
}

int
Tonic_SetRingDisplay(TonicModelContext *ctx, int mode)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetRingDisplay: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetRingDisplay(mode)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetRingDisplay: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetRingDisplay(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetRingDisplay() : -1;
    } catch (...) {
        return -1;
    }
}

int
Tonic_SetAmplifiedHair(TonicModelContext *ctx, int show)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetAmplifiedHair: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetAmplifiedHair(show != 0)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetAmplifiedHair: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetAmplifiedHair(TonicModelContext const *ctx)
{
    try {
        return (ctx && _Impl(ctx)->model.GetAmplifiedHair()) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_SetLevelDisplay(TonicModelContext *ctx, int level, int visible,
                      int xray)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetLevelDisplay: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetLevelDisplay(level, visible != 0,
                                               xray != 0)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetLevelDisplay: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetLevelDisplay(TonicModelContext const *ctx, int level,
                      int *outVisible, int *outXray)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetLevelDisplay: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::LevelDisplay const display =
            _Impl(ctx)->model.GetLevelDisplay(level);
        if (outVisible) {
            *outVisible = display.visible ? 1 : 0;
        }
        if (outXray) {
            *outXray = display.xray ? 1 : 0;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetLevelDisplay: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetFocusLevel(TonicModelContext *ctx, int level)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetFocusLevel: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetFocusLevel(level)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetFocusLevel: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetFocusLevel(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetFocusLevel() : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_SetActiveCutEnabled(TonicModelContext *ctx, int enabled)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetActiveCutEnabled: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetActiveCutEnabled(enabled != 0)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetActiveCutEnabled: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetActiveCutEnabled(TonicModelContext const *ctx)
{
    try {
        return ctx && _Impl(ctx)->model.GetActiveCutEnabled() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_SetTubeExpanded(TonicModelContext *ctx, int tubeId, int expanded)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetTubeExpanded: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetTubeExpanded(tubeId, expanded != 0)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetTubeExpanded: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeExpanded(TonicModelContext const *ctx, int tubeId)
{
    try {
        return ctx && _Impl(ctx)->model.GetTubeExpanded(tubeId) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_IsTubeVisible(TonicModelContext const *ctx, int tubeId)
{
    try {
        return ctx && _Impl(ctx)->model.IsTubeVisibleInActiveCut(tubeId)
                   ? 1
                   : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_SetDisplayScale(TonicModelContext *ctx, float worldPerPixel)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetDisplayScale: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetDisplayScale(worldPerPixel)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetDisplayScale: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetDisplayScale(TonicModelContext const *ctx, float *outWorldPerPixel)
{
    try {
        if (!ctx || !outWorldPerPixel) {
            _SetError("Tonic_GetDisplayScale: null argument");
            return TONIC_ERROR;
        }
        *outWorldPerPixel = _Impl(ctx)->model.GetDisplayScale();
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetDisplayScale: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetDisplayPolicy(TonicModelContext *ctx, char const *mode,
                       char const *subMode, int focusLevel)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetDisplayPolicy: null context");
            return TONIC_ERROR;
        }
        int const displayMode =
            usdGenTonic::TonicDisplayModeFromNames(mode, subMode);
        if (displayMode < 0) {
            _SetError("Tonic_SetDisplayPolicy: unknown mode");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel &model = _Impl(ctx)->model;
        int const maxLevel = std::max(1, model.GetMaxTubeLevel());
        for (int level = 1; level <= maxLevel; ++level) {
            usdGenTonic::TonicModel::LevelDisplay const draw =
                usdGenTonic::TonicPolicyLevelDisplay(displayMode, level,
                                                     focusLevel);
            if (!model.SetLevelDraw(level, draw)) {
                _SetError(model.GetDiagnostic());
                return TONIC_ERROR;
            }
        }
        if (!model.SetRingDisplay(
                usdGenTonic::TonicPolicyRingDisplay(displayMode))) {
            _SetError(model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (!model.SetFocusLevel(std::max(0, focusLevel))) {
            _SetError(model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetDisplayPolicy: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetLevelDraw(TonicModelContext const *ctx, int level,
                   float *outXrayOpacity, int *outCenters)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetLevelDraw: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel::LevelDisplay const draw =
            _Impl(ctx)->model.GetLevelDisplay(level);
        if (outXrayOpacity) {
            *outXrayOpacity = draw.xray ? draw.xrayOpacity : 0.0f;
        }
        if (outCenters) {
            *outCenters = draw.centers ? 1 : 0;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetLevelDraw: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetGroomPath(TonicModelContext *ctx, char const *path)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetGroomPath: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SetGroomPath(path ? std::string(path)
                                                 : std::string())) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetGroomPath: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetGroomPath(TonicModelContext const *ctx, char *out, int cap)
{
    try {
        if (!ctx || !out || cap <= 0) {
            _SetError("Tonic_GetGroomPath: null argument");
            return TONIC_ERROR;
        }
        std::string const path = _Impl(ctx)->model.GetGroomPath();
        if (int(path.size()) + 1 > cap) {
            _SetError("Tonic_GetGroomPath: output too small");
            return TONIC_ERROR;
        }
        std::memcpy(out, path.c_str(), path.size() + 1);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetGroomPath: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetPublishedLevelInfo(TonicModelContext const *ctx, int level,
                            int *outFaceCount, int *outPointCount,
                            int *outTubeCount)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetPublishedLevelInfo: null context");
            return TONIC_ERROR;
        }
        if (!usdGenTonic::TonicRegistry::Get().QueryPublishedLevel(
                level, outFaceCount, outPointCount, outTubeCount)) {
            _SetError("Tonic_GetPublishedLevelInfo: no attached scene index "
                      "publishes that level");
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetPublishedLevelInfo: unknown exception");
        return TONIC_ERROR;
    }
}

/* -- V1: selection, overlays, gestures (plan/18 §2.3-§2.5) --------------- */

namespace {

/* Turn the three parallel id arrays into selection items of one kind.
 * subIds / subSubIds may be NULL for the kinds that do not use them. */
bool
_CollectItems(unsigned int kind, const int *ids, const int *subIds,
              const int *subSubIds, int n,
              std::vector<usdGenTonic::TonicSelectionItem> *out)
{
    if (n < 0 || (n > 0 && !ids)) {
        return false;
    }
    if (kind == 0 || (kind & ~uint32_t(usdGenTonic::TonicSelect_All)) ||
        (kind & (kind - 1)) != 0) {
        return false;  /* exactly one kind per call */
    }
    out->reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        usdGenTonic::TonicSelectionItem item;
        item.kind = kind;
        item.id = ids[i];
        item.subId = subIds ? subIds[i] : -1;
        item.subSubId = subSubIds ? subSubIds[i] : -1;
        out->push_back(item);
    }
    return true;
}

int
_ApplySelect(TonicModelContext *ctx, usdGenTonic::TonicSelectMode mode,
             unsigned int kind, const int *ids, const int *subIds,
             const int *subSubIds, int n, char const *who)
{
    if (!ctx) {
        _SetError(who);
        return TONIC_ERROR;
    }
    std::vector<usdGenTonic::TonicSelectionItem> items;
    if (!_CollectItems(kind, ids, subIds, subSubIds, n, &items)) {
        _SetError(who);
        return TONIC_ERROR;
    }
    _Impl(ctx)->model.SelectionApply(mode, items);
    return TONIC_OK;
}

usdGenTonic::TonicSelectMode
_SelectModeFrom(int mode)
{
    switch (mode) {
    case TONIC_SELECT_ADD:
        return usdGenTonic::TonicSelect_Add;
    case TONIC_SELECT_TOGGLE:
        return usdGenTonic::TonicSelect_Toggle;
    default:
        return usdGenTonic::TonicSelect_Set;
    }
}

}  /* namespace */

int
Tonic_SelectClear(TonicModelContext *ctx, unsigned int kindMask)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SelectClear: null context");
            return TONIC_ERROR;
        }
        _Impl(ctx)->model.SelectionClear(kindMask);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectClear: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SelectSet(TonicModelContext *ctx, unsigned int kind, const int *ids,
                const int *subIds, const int *subSubIds, int n)
{
    try {
        return _ApplySelect(ctx, usdGenTonic::TonicSelect_Set, kind, ids,
                            subIds, subSubIds, n,
                            "Tonic_SelectSet: null context or bad kind");
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectSet: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SelectAdd(TonicModelContext *ctx, unsigned int kind, const int *ids,
                const int *subIds, const int *subSubIds, int n)
{
    try {
        return _ApplySelect(ctx, usdGenTonic::TonicSelect_Add, kind, ids,
                            subIds, subSubIds, n,
                            "Tonic_SelectAdd: null context or bad kind");
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectAdd: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SelectToggle(TonicModelContext *ctx, unsigned int kind, const int *ids,
                   const int *subIds, const int *subSubIds, int n)
{
    try {
        return _ApplySelect(ctx, usdGenTonic::TonicSelect_Toggle, kind, ids,
                            subIds, subSubIds, n,
                            "Tonic_SelectToggle: null context or bad kind");
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectToggle: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SelectRect(TonicModelContext *ctx, const float *viewProj, int w, int h,
                 float x0, float y0, float x1, float y1,
                 unsigned int kindMask, int mode)
{
    try {
        if (!ctx || !viewProj) {
            _SetError("Tonic_SelectRect: null argument");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SelectRect(viewProj, w, h, x0, y0, x1, y1,
                                          kindMask, _SelectModeFrom(mode))) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectRect: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SelectPolygon(TonicModelContext *ctx, const float *viewProj, int w,
                    int h, const float *xy, int n, unsigned int kindMask,
                    int mode)
{
    try {
        if (!ctx || !viewProj || !xy) {
            _SetError("Tonic_SelectPolygon: null argument");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SelectPolygon(viewProj, w, h, xy, n, kindMask,
                                             _SelectModeFrom(mode))) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SelectPolygon: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetHover(TonicModelContext *ctx, unsigned int kind, int id, int subId,
               int subSubId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetHover: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicSelectionItem item;
        if (kind) {
            if ((kind & ~uint32_t(usdGenTonic::TonicSelect_All)) ||
                (kind & (kind - 1)) != 0) {
                _SetError("Tonic_SetHover: hover takes exactly one kind");
                return TONIC_ERROR;
            }
            item.kind = kind;
            item.id = id;
            item.subId = subId;
            item.subSubId = subSubId;
        }
        _Impl(ctx)->model.SelectionSetHover(item);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetHover: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetHover(TonicModelContext const *ctx, unsigned int *outKind,
               int *outId, int *outSubId, int *outSubSubId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetHover: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicSelectionItem const item =
            _Impl(ctx)->model.SelectionHover();
        if (outKind) {
            *outKind = item.kind;
        }
        if (outId) {
            *outId = item.id;
        }
        if (outSubId) {
            *outSubId = item.subId;
        }
        if (outSubSubId) {
            *outSubSubId = item.subSubId;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetHover: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_ReadSelection(TonicModelContext const *ctx, unsigned int kind,
                    int *outIds, int *outSubIds, int *outSubSubIds, int cap,
                    int *outCount)
{
    try {
        if (!ctx) {
            _SetError("Tonic_ReadSelection: null context");
            return TONIC_ERROR;
        }
        std::vector<usdGenTonic::TonicSelectionItem> const items =
            _Impl(ctx)->model.SelectionItems(kind);
        if (outCount) {
            *outCount = int(items.size());
        }
        bool const wantsArrays = outIds || outSubIds || outSubSubIds;
        if (!wantsArrays) {
            return TONIC_OK;  /* sizing call */
        }
        if (cap < int(items.size())) {
            _SetError("Tonic_ReadSelection: buffer too small");
            return TONIC_ERROR;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (outIds) {
                outIds[i] = items[i].id;
            }
            if (outSubIds) {
                outSubIds[i] = items[i].subId;
            }
            if (outSubSubIds) {
                outSubSubIds[i] = items[i].subSubId;
            }
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_ReadSelection: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetSelectionCount(TonicModelContext const *ctx, unsigned int kindMask)
{
    try {
        return ctx ? int(_Impl(ctx)->model.SelectionCount(kindMask)) : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetSelectionBounds(TonicModelContext const *ctx, float *outMin,
                         float *outMax)
{
    try {
        if (!ctx || !outMin || !outMax) {
            _SetError("Tonic_GetSelectionBounds: null argument");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.SelectionBounds(outMin, outMax)) {
            _SetError("Tonic_GetSelectionBounds: nothing selected has a "
                      "position");
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetSelectionBounds: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_PickItem(TonicModelContext *ctx, const float *viewProj, int w, int h,
               float x, float y, float radiusPx, unsigned int kindMask,
               int *outHit, unsigned int *outKind, int *outId, int *outSubId,
               int *outSubSubId)
{
    try {
        if (!ctx || !viewProj || !outHit) {
            _SetError("Tonic_PickItem: null argument");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicPickHit const hit = _Impl(ctx)->model.PickItem(
            viewProj, w, h, x, y, radiusPx, uint32_t(kindMask));
        usdGenTonic::TonicSelectionItem const item =
            _Impl(ctx)->model.SelectionItemFromHit(hit);
        *outHit = (hit.hit && item.kind) ? 1 : 0;
        if (!*outHit) {
            return TONIC_OK;
        }
        if (outKind) {
            *outKind = item.kind;
        }
        if (outId) {
            *outId = item.id;
        }
        if (outSubId) {
            *outSubId = item.subId;
        }
        if (outSubSubId) {
            *outSubSubId = item.subSubId;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_PickItem: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetGizmo(TonicModelContext *ctx, int kind, const float *origin,
               const float *frame, float sizeWorld, int activeHandle)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetGizmo: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicGizmoRecord record;
        record.kind = kind;
        if (origin) {
            for (int i = 0; i < 3; ++i) {
                record.origin[i] = origin[i];
            }
        }
        if (frame) {
            for (int i = 0; i < 9; ++i) {
                record.frame[i] = frame[i];
            }
        }
        record.sizeWorld = sizeWorld;
        record.activeHandle = activeHandle;
        if (!_Impl(ctx)->model.SetGizmo(record)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetGizmo: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetGizmo(TonicModelContext const *ctx, int *outKind, float *outOrigin,
               float *outFrame, float *outSizeWorld, int *outActiveHandle)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetGizmo: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicGizmoRecord const record =
            _Impl(ctx)->model.GetGizmo();
        if (outKind) {
            *outKind = record.kind;
        }
        if (outOrigin) {
            for (int i = 0; i < 3; ++i) {
                outOrigin[i] = record.origin[i];
            }
        }
        if (outFrame) {
            for (int i = 0; i < 9; ++i) {
                outFrame[i] = record.frame[i];
            }
        }
        if (outSizeWorld) {
            *outSizeWorld = record.sizeWorld;
        }
        if (outActiveHandle) {
            *outActiveHandle = record.activeHandle;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetGizmo: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_SetBrushRing(TonicModelContext *ctx, const float *center,
                   const float *normal, float radiusWorld)
{
    try {
        if (!ctx) {
            _SetError("Tonic_SetBrushRing: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicBrushRingRecord record;
        record.active = radiusWorld > 0.0f;
        if (center) {
            for (int i = 0; i < 3; ++i) {
                record.center[i] = center[i];
            }
        }
        if (normal) {
            for (int i = 0; i < 3; ++i) {
                record.normal[i] = normal[i];
            }
        }
        record.radiusWorld = radiusWorld > 0.0f ? radiusWorld : 0.0f;
        if (!_Impl(ctx)->model.SetBrushRing(record)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_SetBrushRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetBrushRing(TonicModelContext const *ctx, int *outActive,
                   float *outCenter, float *outNormal, float *outRadius)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetBrushRing: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicBrushRingRecord const record =
            _Impl(ctx)->model.GetBrushRing();
        if (outActive) {
            *outActive = record.active ? 1 : 0;
        }
        if (outCenter) {
            for (int i = 0; i < 3; ++i) {
                outCenter[i] = record.center[i];
            }
        }
        if (outNormal) {
            for (int i = 0; i < 3; ++i) {
                outNormal[i] = record.normal[i];
            }
        }
        if (outRadius) {
            *outRadius = record.radiusWorld;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetBrushRing: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_BeginGesture(TonicModelContext *ctx, const char *label)
{
    try {
        if (!ctx) {
            _SetError("Tonic_BeginGesture: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.BeginGesture(label)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BeginGesture: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_EndGesture(TonicModelContext *ctx)
{
    try {
        if (!ctx) {
            _SetError("Tonic_EndGesture: null context");
            return TONIC_ERROR;
        }
        if (!_Impl(ctx)->model.EndGesture()) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_EndGesture: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_CancelGesture(TonicModelContext *ctx, unsigned int *outDirty)
{
    try {
        if (!ctx) {
            _SetError("Tonic_CancelGesture: null context");
            return TONIC_ERROR;
        }
        uint32_t dirty = 0;
        if (!_Impl(ctx)->model.CancelGesture(&dirty)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (outDirty) {
            *outDirty = dirty;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_CancelGesture: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetGestureDepth(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetGestureDepth() : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_Redo(TonicModelContext *ctx, unsigned int *outDirty)
{
    try {
        if (!ctx) {
            _SetError("Tonic_Redo: null context");
            return TONIC_ERROR;
        }
        uint32_t dirty = 0;
        if (!_Impl(ctx)->model.Redo(&dirty)) {
            _SetError(_Impl(ctx)->model.GetDiagnostic());
            return TONIC_ERROR;
        }
        if (outDirty) {
            *outDirty = dirty;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Redo: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetRedoDepth(TonicModelContext const *ctx)
{
    try {
        return ctx ? _Impl(ctx)->model.GetRedoDepth() : 0;
    } catch (...) {
        return 0;
    }
}

int
Tonic_GetUndoLabel(TonicModelContext const *ctx, int depth, char *out,
                   int cap)
{
    try {
        if (!ctx || !out || cap <= 0) {
            _SetError("Tonic_GetUndoLabel: null argument");
            return TONIC_ERROR;
        }
        std::string label;
        if (!_Impl(ctx)->model.GetUndoLabel(depth, &label)) {
            _SetError("Tonic_GetUndoLabel: no step at that depth");
            return TONIC_ERROR;
        }
        if (int(label.size()) + 1 > cap) {
            _SetError("Tonic_GetUndoLabel: buffer too small");
            return TONIC_ERROR;
        }
        std::memcpy(out, label.c_str(), label.size() + 1);
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetUndoLabel: unknown exception");
        return TONIC_ERROR;
    }
}

} // extern "C"
