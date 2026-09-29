// usdGenPomade — the stage-contract half of the C ABI (plan/18 V0b).
#include "usdGenPomade/pomadeApiStage.h"

#include "usdGenPomade/pomadeApiImpl.h"
#include "usdGenPomade/pomadeBake.h"
#include "usdGenPomade/pomadeCommit.h"
#include "usdGenPomade/pomadeModel.h"
#include "usdGenPomade/pomadeTube.h"

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
    _tlStageError = what.empty() ? "unknown pomade error" : what;
}

usdGenPomade::PomadeModelContextImpl *_Impl(PomadeModelContext *ctx)
{
    return reinterpret_cast<usdGenPomade::PomadeModelContextImpl *>(ctx);
}

usdGenPomade::PomadeModelContextImpl const *_Impl(PomadeModelContext const *ctx)
{
    return reinterpret_cast<usdGenPomade::PomadeModelContextImpl const *>(ctx);
}

usdGenPomade::PomadeBakeContextImpl *_BImpl(PomadeBakeContext *bake)
{
    return reinterpret_cast<usdGenPomade::PomadeBakeContextImpl *>(bake);
}

// Every entry point has the same shape: check the context, run the model
// call, translate false into POMADE_ERROR with the model's diagnostic, and
// never let an exception cross the boundary.
template <class Fn>
int
_Guard(char const *name, PomadeModelContext *ctx, Fn &&fn)
{
    try {
        if (!ctx) {
            _SetError(std::string(name) + ": null context");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeModel &model = _Impl(ctx)->model;
        if (!fn(model)) {
            std::string const diagnostic = model.GetDiagnostic();
            _SetError(diagnostic.empty() ? std::string(name) + ": failed"
                                         : diagnostic);
            return POMADE_ERROR;
        }
        return POMADE_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError(std::string(name) + ": unknown exception");
        return POMADE_ERROR;
    }
}

// The half both hydrate entry points share once they have a stage.
int
_HydrateStage(PomadeModelContext *ctx, UsdStageRefPtr const &stage,
              SdfPath const &groomPath, int *outTubeCount, int *outGuideCount,
              int *outImportedCount)
{
    usdGenPomade::PomadeHydrateResult const result =
        usdGenPomade::PomadeHydrateModel(stage, groomPath, &_Impl(ctx)->model);
    if (!result.ok) {
        _SetError(result.diagnostic);
        return POMADE_ERROR;
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
    return POMADE_OK;
}

}  // namespace

extern "C" {

const char *
Pomade_StageGetLastError(void)
{
    return _tlStageError.c_str();
}

int
Pomade_Hydrate(PomadeModelContext *ctx, const char *layerOrStage,
              const char *groomPath, int *outTubeCount, int *outGuideCount,
              int *outImportedCount)
{
    try {
        if (!ctx || !layerOrStage || !groomPath) {
            _SetError("Pomade_Hydrate: null argument");
            return POMADE_ERROR;
        }
        SdfPath const path(groomPath);
        if (path.IsEmpty() || !path.IsAbsolutePath()) {
            _SetError("Pomade_Hydrate: groom path must be absolute");
            return POMADE_ERROR;
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
            _SetError(std::string("Pomade_Hydrate: cannot open ") +
                      layerOrStage);
            return POMADE_ERROR;
        }
        return _HydrateStage(ctx, stage, path, outTubeCount, outGuideCount,
                             outImportedCount);
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_Hydrate: unknown exception");
        return POMADE_ERROR;
    }
}

int
Pomade_HydrateFromLayers(PomadeModelContext *ctx,
                        const char *const *layerIdentifiers, int layerCount,
                        const char *groomPath, int *outTubeCount,
                        int *outGuideCount, int *outImportedCount)
{
    try {
        if (!ctx || !layerIdentifiers || layerCount < 1 || !groomPath) {
            _SetError("Pomade_HydrateFromLayers: null argument or no layers");
            return POMADE_ERROR;
        }
        SdfPath const path(groomPath);
        if (path.IsEmpty() || !path.IsAbsolutePath()) {
            _SetError("Pomade_HydrateFromLayers: groom path must be absolute");
            return POMADE_ERROR;
        }
        // Held here for the whole call: an anonymous session layer made
        // below names the others only by identifier, and SdfLayer::Find
        // can only resolve a layer something keeps alive.
        std::vector<SdfLayerRefPtr> layers;
        layers.reserve(size_t(layerCount));
        for (int i = 0; i < layerCount; ++i) {
            char const *id = layerIdentifiers[i];
            if (!id || !*id) {
                _SetError("Pomade_HydrateFromLayers: empty layer identifier");
                return POMADE_ERROR;
            }
            SdfLayerRefPtr layer = SdfLayer::Find(std::string(id));
            if (!layer) {
                layer = SdfLayer::FindOrOpen(std::string(id));
            }
            if (!layer) {
                _SetError(std::string("Pomade_HydrateFromLayers: cannot open ") +
                          id);
                return POMADE_ERROR;
            }
            layers.push_back(layer);
        }
        // The stage usdview shows is its root layer composed under its
        // session layer. A groom saved beside the session's live overlay
        // lives only in the session layer's stack, and the scalp mesh it
        // points at usually lives only in the root layer's, so neither
        // layer alone can hydrate it (SS-03).
        UsdStageRefPtr stage;
        if (layers.size() == 1) {
            stage = UsdStage::Open(layers[0]);
        } else if (layers.size() == 2) {
            stage = UsdStage::Open(layers[0], layers[1]);
        } else {
            SdfLayerRefPtr const session =
                SdfLayer::CreateAnonymous("usdGenPomade-hydrate-session");
            std::vector<std::string> subLayers;
            for (size_t i = 1; i < layers.size(); ++i) {
                subLayers.push_back(layers[i]->GetIdentifier());
            }
            session->SetSubLayerPaths(subLayers);
            stage = UsdStage::Open(layers[0], session);
        }
        if (!stage) {
            _SetError(std::string("Pomade_HydrateFromLayers: cannot compose ") +
                      layerIdentifiers[0]);
            return POMADE_ERROR;
        }
        return _HydrateStage(ctx, stage, path, outTubeCount, outGuideCount,
                             outImportedCount);
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_HydrateFromLayers: unknown exception");
        return POMADE_ERROR;
    }
}

int
Pomade_InsertTubeCenterCV(PomadeModelContext *ctx, int tubeId, int atIndex)
{
    return _Guard("Pomade_InsertTubeCenterCV", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.InsertTubeCenterCV(tubeId, atIndex);
                  });
}

int
Pomade_DeleteTubeCenterCV(PomadeModelContext *ctx, int tubeId, int index)
{
    return _Guard("Pomade_DeleteTubeCenterCV", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.DeleteTubeCenterCV(tubeId, index);
                  });
}

int
Pomade_SetTubeLengthFor(PomadeModelContext *ctx, int tubeId, float length)
{
    return _Guard("Pomade_SetTubeLengthFor", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.SetTubeLengthFor(tubeId, length);
                  });
}

int
Pomade_MatchTubeSurface(PomadeModelContext *ctx, int tubeId)
{
    return _Guard(
        "Pomade_MatchTubeSurface", ctx,
        [&](usdGenPomade::PomadeModel &m) { return m.MatchTubeSurface(tubeId); });
}

int
Pomade_SnapTubeRootToScalp(PomadeModelContext *ctx, int tubeId)
{
    return _Guard("Pomade_SnapTubeRootToScalp", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.SnapTubeRootToScalp(tubeId);
                  });
}

int
Pomade_RelaxTubeCenter(PomadeModelContext *ctx, int tubeId, float strength,
                      int iterations)
{
    return _Guard("Pomade_RelaxTubeCenter", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.RelaxTubeCenter(tubeId, strength, iterations);
                  });
}

int
Pomade_MoveTubeSectionRing(PomadeModelContext *ctx, int tubeId, int ring,
                          float du, float dv)
{
    return _Guard("Pomade_MoveTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.MoveTubeSectionRing(tubeId, ring, du, dv);
                  });
}

int
Pomade_ScaleTubeSectionRing(PomadeModelContext *ctx, int tubeId, int ring,
                           float scale)
{
    return _Guard("Pomade_ScaleTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.ScaleTubeSectionRing(tubeId, ring, scale);
                  });
}

int
Pomade_TwistTubeSectionRing(PomadeModelContext *ctx, int tubeId, int ring,
                           float radians)
{
    return _Guard("Pomade_TwistTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.TwistTubeSectionRing(tubeId, ring, radians);
                  });
}

int
Pomade_MoveTubeSectionCV(PomadeModelContext *ctx, int tubeId, int ring, int slot,
                        float du, float dv)
{
    return _Guard("Pomade_MoveTubeSectionCV", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.MoveTubeSectionCV(tubeId, ring, slot, du, dv);
                  });
}

int
Pomade_AddTubeSectionRing(PomadeModelContext *ctx, int tubeId, float t)
{
    return _Guard("Pomade_AddTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.AddTubeSectionRing(tubeId, t);
                  });
}

int
Pomade_RemoveTubeSectionRing(PomadeModelContext *ctx, int tubeId, int ring)
{
    return _Guard("Pomade_RemoveTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.RemoveTubeSectionRing(tubeId, ring);
                  });
}

int
Pomade_CopyTubeSectionRing(PomadeModelContext *ctx, int tubeId, int src, int dst)
{
    return _Guard("Pomade_CopyTubeSectionRing", ctx,
                  [&](usdGenPomade::PomadeModel &m) {
                      return m.CopyTubeSectionRing(tubeId, src, dst);
                  });
}

int
Pomade_SetTubeFillParams(PomadeModelContext *ctx, int tubeId, float density,
                        int cvCount, int seed, float edgeBias,
                        const float *profile, int profileCount)
{
    return _Guard(
        "Pomade_SetTubeFillParams", ctx, [&](usdGenPomade::PomadeModel &m) {
            usdGenPomade::PomadeModel::FillParams params;
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
Pomade_GetTubeFillParams(PomadeModelContext *ctx, int tubeId, float *outDensity,
                        int *outCvCount, int *outSeed, float *outEdgeBias,
                        int *outProfileCount)
{
    return _Guard(
        "Pomade_GetTubeFillParams", ctx, [&](usdGenPomade::PomadeModel &m) {
            usdGenPomade::PomadeModel::FillParams params;
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
Pomade_IsTubeFillSuspended(PomadeModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Pomade_IsTubeFillSuspended: null context");
            return -1;
        }
        return _Impl(ctx)->model.IsTubeFillSuspended(tubeId) ? 1 : 0;
    } catch (...) {
        _SetError("Pomade_IsTubeFillSuspended: unknown exception");
        return -1;
    }
}

int
Pomade_GetTubeParent(PomadeModelContext const *ctx, int tubeId, int *outParent,
                    int *outChildIndex)
{
    try {
        if (!ctx) {
            _SetError("Pomade_GetTubeParent: null context");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeTubeDesc desc;
        if (!_Impl(ctx)->model.GetTubeDesc(tubeId, &desc)) {
            _SetError("Pomade_GetTubeParent: unknown tube");
            return POMADE_ERROR;
        }
        if (outParent) {
            *outParent = desc.parentTubeId;
        }
        if (outChildIndex) {
            *outChildIndex = desc.childIndex;
        }
        return POMADE_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_GetTubeParent: unknown exception");
        return POMADE_ERROR;
    }
}

int
Pomade_IsTubePersistent(PomadeModelContext const *ctx, int tubeId)
{
    try {
        if (!ctx) {
            _SetError("Pomade_IsTubePersistent: null context");
            return -1;
        }
        usdGenPomade::PomadeModel::TubeRecord record;
        if (!_Impl(ctx)->model.GetTubeRecord(tubeId, &record)) {
            _SetError("Pomade_IsTubePersistent: unknown tube");
            return -1;
        }
        return record.persistent ? 1 : 0;
    } catch (...) {
        _SetError("Pomade_IsTubePersistent: unknown exception");
        return -1;
    }
}

int
Pomade_RemoveTubes(PomadeModelContext *ctx, const int *ids, int n)
{
    if (!ids || n <= 0) {
        _SetError("Pomade_RemoveTubes: no tube ids");
        return POMADE_ERROR;
    }
    return _Guard("Pomade_RemoveTubes", ctx, [&](usdGenPomade::PomadeModel &m) {
        return m.RemoveTubes(std::vector<int>(ids, ids + n), nullptr);
    });
}

int
Pomade_BakeEnqueueLevels(PomadeBakeContext *bake)
{
    try {
        if (!bake) {
            _SetError("Pomade_BakeEnqueueLevels: null bake context");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeBakeContextImpl *impl = _BImpl(bake);
        if (!impl->model || !impl->model->HasScalp()) {
            _SetError("Pomade_BakeEnqueueLevels: no scalp bound");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeBakeInput input;
        input.scalp = impl->model->GetScalp();
        input.graph = impl->model->GetGraph();
        input.levelCount = impl->levelCount;
        input.resOverride = impl->resOverride;
        input.outDir = impl->outDir;
        input.baseName = impl->baseName;
        usdGenPomade::PomadeCollectBakeTubes(*impl->model, &input.tubes);
        impl->worker->Enqueue(impl->model->GetMapVersion(), std::move(input));
        return POMADE_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_BakeEnqueueLevels: unknown exception");
        return POMADE_ERROR;
    }
}

int
Pomade_GetTubeSectionFrame(PomadeModelContext *ctx, int tubeId, int ring,
                          float *outOrigin, float *outFrame, float *outScale,
                          float *outTwist)
{
    try {
        if (!ctx) {
            _SetError("Pomade_GetTubeSectionFrame: null context");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeTubeDesc desc;
        if (!_Impl(ctx)->model.GetTubeDesc(tubeId, &desc)) {
            _SetError("Pomade_GetTubeSectionFrame: unknown tube");
            return POMADE_ERROR;
        }
        if (ring < 0 || ring >= int(desc.sections.size()) ||
            desc.centerX.size() < 2) {
            _SetError("Pomade_GetTubeSectionFrame: ring out of range");
            return POMADE_ERROR;
        }
        // The same three steps the K11 section candidates take
        // (pomadeModel.cpp _BuildPickSetsLocked): K4 frames over the center
        // column, the center point and the nlerped frame at the ring's t,
        // then the ring's own scale and twist.
        std::vector<usdGenPomade::PomadeFrame> frames;
        std::string err;
        if (!usdGenPomade::PomadeTubeFramesCpu(desc, &frames, &err) ||
            frames.empty()) {
            _SetError(err.empty() ? "Pomade_GetTubeSectionFrame: no frames"
                                  : err);
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeTubeSection const &s = desc.sections[size_t(ring)];
        float center[3] = {0.0f, 0.0f, 0.0f};
        usdGenPomade::PomadeEvalCenter(desc.centerX.data(), desc.centerY.data(),
                                     desc.centerZ.data(),
                                     int(desc.centerX.size()), s.t, center);
        usdGenPomade::PomadeFrame fr;
        usdGenPomade::PomadeNlerpFrame(frames.data(), int(frames.size()), s.t,
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
        return POMADE_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_GetTubeSectionFrame: unknown exception");
        return POMADE_ERROR;
    }
}


int
Pomade_SculptStrokeShaped(PomadeModelContext *ctx, int tubeId,
                         const char *brush, const float *viewProj, int w,
                         int h, float x, float y, float radiusPx,
                         const float *deltaWorld, float amount,
                         float tCenter, float tRadius, int preserveLength,
                         int mirrorX, int *outTouched)
{
    return _Guard("Pomade_SculptStrokeShaped", ctx,
                  [&](usdGenPomade::PomadeModel &model) {
                      return model.SculptStrokeShaped(
                          tubeId, brush, viewProj, w, h, x, y, radiusPx,
                          deltaWorld, amount, tCenter, tRadius,
                          preserveLength != 0, mirrorX != 0, outTouched);
                  });
}

int
Pomade_SubdivideTubeEdge(PomadeModelContext *ctx, int tubeId,
                        const float *worldA, const float *worldB, int seed,
                        int *outIds, int outCap, int *outCount)
{
    std::vector<int> kids;
    int const status =
        _Guard("Pomade_SubdivideTubeEdge", ctx,
               [&](usdGenPomade::PomadeModel &model) {
                   return model.SubdivideTubeAlongEdge(tubeId, worldA, worldB,
                                                       seed, &kids);
               });
    if (status != POMADE_OK) {
        return status;
    }
    if (outCount) {
        *outCount = int(kids.size());
    }
    if (outIds) {
        if (outCap < int(kids.size())) {
            _SetError("Pomade_SubdivideTubeEdge: output too small");
            return POMADE_ERROR;
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            outIds[i] = kids[i];
        }
    }
    return POMADE_OK;
}

int
Pomade_SetLevelDrawMode(PomadeModelContext *ctx, int level, int visible,
                       int xray, int centersOnly)
{
    return _Guard("Pomade_SetLevelDrawMode", ctx,
                  [&](usdGenPomade::PomadeModel &model) {
                      return model.SetLevelDrawMode(level, visible != 0,
                                                    xray != 0,
                                                    centersOnly != 0);
                  });
}

int
Pomade_GetLevelDrawMode(PomadeModelContext const *ctx, int level,
                       int *outVisible, int *outXray, int *outCentersOnly)
{
    try {
        if (!ctx) {
            _SetError("Pomade_GetLevelDrawMode: null context");
            return POMADE_ERROR;
        }
        if (level < 1) {
            _SetError("Pomade_GetLevelDrawMode: level must be >= 1");
            return POMADE_ERROR;
        }
        usdGenPomade::PomadeModel::LevelDisplay const display =
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
        return POMADE_OK;
    } catch (std::exception const &e) {
        _SetError(e.what());
        return POMADE_ERROR;
    } catch (...) {
        _SetError("Pomade_GetLevelDrawMode: unknown exception");
        return POMADE_ERROR;
    }
}

}  // extern "C"
