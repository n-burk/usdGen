// usdGenTonic — the stage-contract half of the C ABI (plan/18 V0b).
#include "usdGenTonic/tonicApiStage.h"

#include "usdGenTonic/tonicApiImpl.h"
#include "usdGenTonic/tonicBake.h"
#include "usdGenTonic/tonicCommit.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicTube.h"

#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

#include <cmath>
#include <exception>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

thread_local std::string _tlStageError;

void _SetError(std::string const &what)
{
    _tlStageError = what.empty() ? "unknown tonic error" : what;
}

usdGenTonic::TonicModelContextImpl *_Impl(TonicModelContext *ctx)
{
    return reinterpret_cast<usdGenTonic::TonicModelContextImpl *>(ctx);
}

usdGenTonic::TonicModelContextImpl const *_Impl(TonicModelContext const *ctx)
{
    return reinterpret_cast<usdGenTonic::TonicModelContextImpl const *>(ctx);
}

usdGenTonic::TonicBakeContextImpl *_BImpl(TonicBakeContext *bake)
{
    return reinterpret_cast<usdGenTonic::TonicBakeContextImpl *>(bake);
}

// Every entry point has the same shape: check the context, run the model
// call, translate false into TONIC_ERROR with the model's diagnostic, and
// never let an exception cross the boundary.
template <class Fn>
int
_Guard(char const *name, TonicModelContext *ctx, Fn &&fn)
{
    try {
        if (!ctx) {
            _SetError(std::string(name) + ": null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicModel &model = _Impl(ctx)->model;
        if (!fn(model)) {
            std::string const diagnostic = model.GetDiagnostic();
            _SetError(diagnostic.empty() ? std::string(name) + ": failed"
                                         : diagnostic);
            return TONIC_ERROR;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError(std::string(name) + ": unknown exception");
        return TONIC_ERROR;
    }
}

}  // namespace

extern "C" {

const char *
Tonic_StageGetLastError(void)
{
    return _tlStageError.c_str();
}

int
Tonic_Hydrate(TonicModelContext *ctx, const char *layerOrStage,
              const char *groomPath, int *outTubeCount, int *outGuideCount,
              int *outImportedCount)
{
    try {
        if (!ctx || !layerOrStage || !groomPath) {
            _SetError("Tonic_Hydrate: null argument");
            return TONIC_ERROR;
        }
        SdfPath const path(groomPath);
        if (path.IsEmpty() || !path.IsAbsolutePath()) {
            _SetError("Tonic_Hydrate: groom path must be absolute");
            return TONIC_ERROR;
        }
        // An identifier of a layer this process already holds (the live
        // sublayer, an anonymous test layer) wins over the file system, so
        // the tool can hydrate from what it just committed.
        UsdStageRefPtr stage;
        if (SdfLayerHandle const layer =
                SdfLayer::Find(std::string(layerOrStage))) {
            stage = UsdStage::Open(layer);
        } else {
            stage = UsdStage::Open(std::string(layerOrStage));
        }
        if (!stage) {
            _SetError(std::string("Tonic_Hydrate: cannot open ") +
                      layerOrStage);
            return TONIC_ERROR;
        }
        usdGenTonic::TonicHydrateResult const result =
            usdGenTonic::TonicHydrateModel(stage, path, &_Impl(ctx)->model);
        if (!result.ok) {
            _SetError(result.diagnostic);
            return TONIC_ERROR;
        }
        if (outTubeCount) {
            *outTubeCount = int(result.tubeCount);
        }
        if (outGuideCount) {
            *outGuideCount = int(result.guideCount);
        }
        if (outImportedCount) {
            *outImportedCount = int(result.importedTubeCount);
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_Hydrate: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_InsertTubeCenterCV(TonicModelContext *ctx, int tubeId, int atIndex)
{
    return _Guard("Tonic_InsertTubeCenterCV", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.InsertTubeCenterCV(tubeId, atIndex);
                  });
}

int
Tonic_DeleteTubeCenterCV(TonicModelContext *ctx, int tubeId, int index)
{
    return _Guard("Tonic_DeleteTubeCenterCV", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.DeleteTubeCenterCV(tubeId, index);
                  });
}

int
Tonic_SetTubeLengthFor(TonicModelContext *ctx, int tubeId, float length)
{
    return _Guard("Tonic_SetTubeLengthFor", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.SetTubeLengthFor(tubeId, length);
                  });
}

int
Tonic_MatchTubeSurface(TonicModelContext *ctx, int tubeId)
{
    return _Guard(
        "Tonic_MatchTubeSurface", ctx,
        [&](usdGenTonic::TonicModel &m) { return m.MatchTubeSurface(tubeId); });
}

int
Tonic_SnapTubeRootToScalp(TonicModelContext *ctx, int tubeId)
{
    return _Guard("Tonic_SnapTubeRootToScalp", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.SnapTubeRootToScalp(tubeId);
                  });
}

int
Tonic_RelaxTubeCenter(TonicModelContext *ctx, int tubeId, float strength,
                      int iterations)
{
    return _Guard("Tonic_RelaxTubeCenter", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.RelaxTubeCenter(tubeId, strength, iterations);
                  });
}

int
Tonic_MoveTubeSectionRing(TonicModelContext *ctx, int tubeId, int ring,
                          float du, float dv)
{
    return _Guard("Tonic_MoveTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.MoveTubeSectionRing(tubeId, ring, du, dv);
                  });
}

int
Tonic_ScaleTubeSectionRing(TonicModelContext *ctx, int tubeId, int ring,
                           float scale)
{
    return _Guard("Tonic_ScaleTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.ScaleTubeSectionRing(tubeId, ring, scale);
                  });
}

int
Tonic_TwistTubeSectionRing(TonicModelContext *ctx, int tubeId, int ring,
                           float radians)
{
    return _Guard("Tonic_TwistTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.TwistTubeSectionRing(tubeId, ring, radians);
                  });
}

int
Tonic_MoveTubeSectionCV(TonicModelContext *ctx, int tubeId, int ring, int slot,
                        float du, float dv)
{
    return _Guard("Tonic_MoveTubeSectionCV", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.MoveTubeSectionCV(tubeId, ring, slot, du, dv);
                  });
}

int
Tonic_AddTubeSectionRing(TonicModelContext *ctx, int tubeId, float t)
{
    return _Guard("Tonic_AddTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.AddTubeSectionRing(tubeId, t);
                  });
}

int
Tonic_RemoveTubeSectionRing(TonicModelContext *ctx, int tubeId, int ring)
{
    return _Guard("Tonic_RemoveTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.RemoveTubeSectionRing(tubeId, ring);
                  });
}

int
Tonic_CopyTubeSectionRing(TonicModelContext *ctx, int tubeId, int src, int dst)
{
    return _Guard("Tonic_CopyTubeSectionRing", ctx,
                  [&](usdGenTonic::TonicModel &m) {
                      return m.CopyTubeSectionRing(tubeId, src, dst);
                  });
}

int
Tonic_SetTubeFillParams(TonicModelContext *ctx, int tubeId, float density,
                        int cvCount, int seed, float edgeBias,
                        const float *profile, int profileCount)
{
    return _Guard(
        "Tonic_SetTubeFillParams", ctx, [&](usdGenTonic::TonicModel &m) {
            usdGenTonic::TonicModel::FillParams params;
            params.density = density;
            params.cvCount = cvCount;
            params.seed = seed;
            params.edgeBias = edgeBias;
            if (profile && profileCount > 0) {
                params.lengthProfile.assign(profile, profile + profileCount);
            }
            return m.SetTubeFillParams(tubeId, std::move(params));
        });
}

int
Tonic_GetTubeFillParams(TonicModelContext *ctx, int tubeId, float *outDensity,
                        int *outCvCount, int *outSeed, float *outEdgeBias,
                        int *outProfileCount)
{
    return _Guard(
        "Tonic_GetTubeFillParams", ctx, [&](usdGenTonic::TonicModel &m) {
            usdGenTonic::TonicModel::FillParams params;
            if (!m.GetTubeFillParams(tubeId, &params)) {
                return false;
            }
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
            if (outProfileCount) {
                *outProfileCount = int(params.lengthProfile.size());
            }
            return true;
        });
}

int
Tonic_IsTubeFillSuspended(TonicModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_IsTubeFillSuspended: null context");
            return -1;
        }
        return _Impl(ctx)->model.IsTubeFillSuspended(tubeId) ? 1 : 0;
    } catch (...) {
        _SetError("Tonic_IsTubeFillSuspended: unknown exception");
        return -1;
    }
}

int
Tonic_GetTubeParent(TonicModelContext const *ctx, int tubeId, int *outParent,
                    int *outChildIndex)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetTubeParent: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeDesc desc;
        if (!_Impl(ctx)->model.GetTubeDesc(tubeId, &desc)) {
            _SetError("Tonic_GetTubeParent: unknown tube");
            return TONIC_ERROR;
        }
        if (outParent) {
            *outParent = desc.parentTubeId;
        }
        if (outChildIndex) {
            *outChildIndex = desc.childIndex;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeParent: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_IsTubePersistent(TonicModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Tonic_IsTubePersistent: null context");
            return -1;
        }
        usdGenTonic::TonicModel::TubeRecord record;
        if (!_Impl(ctx)->model.GetTubeRecord(tubeId, &record)) {
            _SetError("Tonic_IsTubePersistent: unknown tube");
            return -1;
        }
        return record.persistent ? 1 : 0;
    } catch (...) {
        _SetError("Tonic_IsTubePersistent: unknown exception");
        return -1;
    }
}

int
Tonic_BakeEnqueueLevels(TonicBakeContext *bake)
{
    try {
        if (!bake) {
            _SetError("Tonic_BakeEnqueueLevels: null bake context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicBakeContextImpl *impl = _BImpl(bake);
        if (!impl->model || !impl->model->HasScalp()) {
            _SetError("Tonic_BakeEnqueueLevels: no scalp bound");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicBakeInput input;
        input.scalp = impl->model->GetScalp();
        input.graph = impl->model->GetGraph();
        input.levelCount = impl->levelCount;
        input.resOverride = impl->resOverride;
        input.outDir = impl->outDir;
        input.baseName = impl->baseName;
        usdGenTonic::TonicCollectBakeTubes(*impl->model, &input.tubes);
        impl->worker->Enqueue(impl->model->GetMapVersion(), std::move(input));
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_BakeEnqueueLevels: unknown exception");
        return TONIC_ERROR;
    }
}

int
Tonic_GetTubeSectionFrame(TonicModelContext *ctx, int tubeId, int ring,
                          float *outOrigin, float *outFrame, float *outScale,
                          float *outTwist)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetTubeSectionFrame: null context");
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeDesc desc;
        if (!_Impl(ctx)->model.GetTubeDesc(tubeId, &desc)) {
            _SetError("Tonic_GetTubeSectionFrame: unknown tube");
            return TONIC_ERROR;
        }
        if (ring < 0 || ring >= int(desc.sections.size()) ||
            desc.centerX.size() < 2) {
            _SetError("Tonic_GetTubeSectionFrame: ring out of range");
            return TONIC_ERROR;
        }
        // The same three steps the K11 section candidates take
        // (tonicModel.cpp _BuildPickSetsLocked): K4 frames over the center
        // column, the center point and the nlerped frame at the ring's t,
        // then the ring's own scale and twist.
        std::vector<usdGenTonic::TonicFrame> frames;
        std::string err;
        if (!usdGenTonic::TonicTubeFramesCpu(desc, &frames, &err) ||
            frames.empty()) {
            _SetError(err.empty() ? "Tonic_GetTubeSectionFrame: no frames"
                                  : err);
            return TONIC_ERROR;
        }
        usdGenTonic::TonicTubeSection const &s = desc.sections[size_t(ring)];
        float center[3] = {0.0f, 0.0f, 0.0f};
        usdGenTonic::TonicEvalCenter(desc.centerX.data(), desc.centerY.data(),
                                     desc.centerZ.data(),
                                     int(desc.centerX.size()), s.t, center);
        usdGenTonic::TonicFrame fr;
        usdGenTonic::TonicNlerpFrame(frames.data(), int(frames.size()), s.t,
                                     &fr);
        if (outFrame) {
            outFrame[0] = fr.nx;
            outFrame[1] = fr.ny;
            outFrame[2] = fr.nz;
            outFrame[3] = fr.bx;
            outFrame[4] = fr.by;
            outFrame[5] = fr.bz;
            outFrame[6] = fr.tx;
            outFrame[7] = fr.ty;
            outFrame[8] = fr.tz;
        }
        if (outOrigin) {
            // The ring's centroid, not the center point: an off-centre ring
            // must carry its gizmo with it, and the centroid is what the
            // pick reports a ring by.
            double const ct = std::cos(double(s.twist));
            double const st = std::sin(double(s.twist));
            double mu = 0.0, mv = 0.0;
            size_t const n = s.u.size();
            for (size_t i = 0; i < n; ++i) {
                double const uu = double(s.u[i]) * double(s.scale);
                double const vv = double(s.v[i]) * double(s.scale);
                mu += uu * ct - vv * st;
                mv += uu * st + vv * ct;
            }
            if (n) {
                mu /= double(n);
                mv /= double(n);
            }
            outOrigin[0] = center[0] + fr.nx * float(mu) + fr.bx * float(mv);
            outOrigin[1] = center[1] + fr.ny * float(mu) + fr.by * float(mv);
            outOrigin[2] = center[2] + fr.nz * float(mu) + fr.bz * float(mv);
        }
        if (outScale) {
            *outScale = s.scale;
        }
        if (outTwist) {
            *outTwist = s.twist;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetTubeSectionFrame: unknown exception");
        return TONIC_ERROR;
    }
}


int
Tonic_SculptStrokeShaped(TonicModelContext *ctx, int tubeId,
                         const char *brush, const float *viewProj, int w,
                         int h, float x, float y, float radiusPx,
                         const float *deltaWorld, float amount,
                         float tCenter, float tRadius, int preserveLength,
                         int mirrorX, int *outTouched)
{
    return _Guard("Tonic_SculptStrokeShaped", ctx,
                  [&](usdGenTonic::TonicModel &model) {
                      return model.SculptStrokeShaped(
                          tubeId, brush, viewProj, w, h, x, y, radiusPx,
                          deltaWorld, amount, tCenter, tRadius,
                          preserveLength != 0, mirrorX != 0, outTouched);
                  });
}

int
Tonic_SubdivideTubeEdge(TonicModelContext *ctx, int tubeId,
                        const float *worldA, const float *worldB, int seed,
                        int *outIds, int outCap, int *outCount)
{
    std::vector<int> kids;
    int const status =
        _Guard("Tonic_SubdivideTubeEdge", ctx,
               [&](usdGenTonic::TonicModel &model) {
                   return model.SubdivideTubeAlongEdge(tubeId, worldA, worldB,
                                                       seed, &kids);
               });
    if (status != TONIC_OK) {
        return status;
    }
    if (outCount) {
        *outCount = int(kids.size());
    }
    if (outIds) {
        if (outCap < int(kids.size())) {
            _SetError("Tonic_SubdivideTubeEdge: output too small");
            return TONIC_ERROR;
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            outIds[i] = kids[i];
        }
    }
    return TONIC_OK;
}

int
Tonic_SetLevelDrawMode(TonicModelContext *ctx, int level, int visible,
                       int xray, int centersOnly)
{
    return _Guard("Tonic_SetLevelDrawMode", ctx,
                  [&](usdGenTonic::TonicModel &model) {
                      return model.SetLevelDrawMode(level, visible != 0,
                                                    xray != 0,
                                                    centersOnly != 0);
                  });
}

int
Tonic_GetLevelDrawMode(TonicModelContext const *ctx, int level,
                       int *outVisible, int *outXray, int *outCentersOnly)
{
    try {
        if (!ctx) {
            _SetError("Tonic_GetLevelDrawMode: null context");
            return TONIC_ERROR;
        }
        if (level < 1) {
            _SetError("Tonic_GetLevelDrawMode: level must be >= 1");
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
        if (outCentersOnly) {
            *outCentersOnly = display.centersOnly ? 1 : 0;
        }
        return TONIC_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return TONIC_ERROR;
    } catch (...) {
        _SetError("Tonic_GetLevelDrawMode: unknown exception");
        return TONIC_ERROR;
    }
}

}  // extern "C"
