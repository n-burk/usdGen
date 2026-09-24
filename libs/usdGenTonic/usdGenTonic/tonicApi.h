/* usdGenTonic/tonicApi.h — the C ABI the Tonic authoring tools call.
 *
 * Control and scalars only; no array ever crosses per element (the
 * 08-tools.md C4 rule, applied to the Tonic lane). Every entry point is
 * extern "C", returns int status (0 = success, non-zero = error) except the
 * const char * accessor, and never throws across the boundary.
 * Tonic_GetLastError() returns the message for the calling thread.
 *
 * Scope, in the order it was built: model lifetime and the static test
 * tube; the commit pipeline (committer create/enqueue/swap/status); the
 * scalp graph, regions and the Ptex bake; Tube and Fill mode; the tube
 * hierarchy; picking and undo; the bridges; the V0 viewport publication
 * (registry, activate, publish, level display) that joins all of it to
 * what the artist sees; and, at the end of the file, the V1 interaction
 * surface (selection, marquee and lasso, hover, the gizmo and brush-ring
 * overlays, the gesture bracket, redo and undo labels).
 */
#ifndef USDGEN_TONIC_API_C_ABI_H
#define USDGEN_TONIC_API_C_ABI_H

#include "usdGenTonic/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (08-tools.md C4: 0 = success, non-zero = error). */
#define TONIC_OK               0
#define TONIC_ERROR            1  /* bad args / engine error (see GetLastError)   */
#define TONIC_NOT_IMPLEMENTED  2  /* no C++ target on this build yet — honest fail */

/* Opaque model handle. */
typedef struct TonicModelContext TonicModelContext;

/* Create a model holding no tube. *outCtx receives the handle. */
int USDGENTONIC_API Tonic_Create(TonicModelContext **outCtx);
/* Destroy a model created by Tonic_Create. NULL is a no-op success. */
int USDGENTONIC_API Tonic_Destroy(TonicModelContext *ctx);

/* Build the static test tube: `rings` cross-sections of `ringVerts`
 * vertices, `radius`, `length` along +Y. Non-positive values select the
 * defaults (5, 8, 0.5, 4.0). */
int USDGENTONIC_API Tonic_BuildTestTube(TonicModelContext *ctx,
                                        int rings, int ringVerts,
                                        float radius, float length);

/* Translate one center ring in the section plane (test-tube sculpt stub). */
int USDGENTONIC_API Tonic_MoveCenterRing(TonicModelContext *ctx,
                                         int ring, float dx, float dz);

/* The model's version (bumps per sealed mutation) and pending dirty bits
 * (TonicDirty_* from tonicModel.h, as an int). */
unsigned long long USDGENTONIC_API Tonic_GetVersion(
    TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_TakeDirty(TonicModelContext *ctx);

/* Tube census for staging-size queries. Zero when no tube is built. */
int USDGENTONIC_API Tonic_GetVertexCount(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_GetQuadCount(TonicModelContext const *ctx);

/* Copy the staged tube positions (3 floats per vertex, ring-major) into
 * `outXYZ`, which must hold at least 3 * Tonic_GetVertexCount(ctx) floats.
 * Stages through the pinned host path, so this observes the device lane
 * where one exists. */
int USDGENTONIC_API Tonic_ReadTubePoints(TonicModelContext *ctx,
                                         float *outXYZ, int xyzLen);

/* 1 when the model holds a CUDA device mirror, else 0. */
int USDGENTONIC_API Tonic_HasCudaMirror(TonicModelContext const *ctx);

/* The P6 OOM-fallback reason: "" while the mirror is healthy (or the
 * context is null), otherwise why the mirror was dropped and the model
 * continues CPU-only. Valid for the model's lifetime (sticky). */
const char *USDGENTONIC_API Tonic_GetDeviceFallbackReason(
    TonicModelContext const *ctx);

/* P6 undo: restore the most recent pre-mutation snapshot (tube +
 * hierarchy + guides, and from V1 the scalp graph too). An empty stack is
 * a no-op success. Bumps the model version so workers re-derive, writes
 * the dirty bits the caller must publish into `outDirty` (NULL to
 * ignore), and leaves the state it replaced on the redo stack. */
int USDGENTONIC_API Tonic_Undo(TonicModelContext *ctx,
                               unsigned int *outDirty);
/* Snapshots currently held and their accounted bytes (0 on null). */
int USDGENTONIC_API Tonic_GetUndoDepth(TonicModelContext const *ctx);
unsigned long long USDGENTONIC_API
Tonic_GetUndoBytes(TonicModelContext const *ctx);
/* Budget: max snapshots + max bytes (either bound evicts oldest;
 * depth 0 disables and clears). Applies immediately. */
int USDGENTONIC_API Tonic_SetUndoBudget(TonicModelContext *ctx,
                                        int maxDepth,
                                        unsigned long long maxBytes);
int USDGENTONIC_API Tonic_ClearUndo(TonicModelContext *ctx);

/* P2 scalp graph + regions (plan/17 section 5.1, K1-K3/G1). Every edit
 * bumps the model version AND the map version; K3 runs only in
 * Tonic_Rasterise (gesture end, section 4.2), never inside an edit. Nodes
 * are addressed by stable model ids; (faceId, u, v) locates surface points
 * in the TonicFacePosition encoding. */
int USDGENTONIC_API Tonic_BindScalp(TonicModelContext *ctx,
                                    float const *points, int pointFloats,
                                    int const *faceVertexCounts, int faceCount,
                                    int const *faceVertexIndices,
                                    int indexCount);
int USDGENTONIC_API Tonic_HasScalp(TonicModelContext const *ctx);

/* K1 queries. *outHit is 1 on a hit (face/uv/position/normal written) and
 * 0 on a miss (outputs untouched). */
int USDGENTONIC_API Tonic_Raycast(TonicModelContext *ctx,
                                  float const origin[3], float const dir[3],
                                  int *outHit, int *outFace, float outUV[2],
                                  float outP[3], float outN[3]);
int USDGENTONIC_API Tonic_ClosestPoint(TonicModelContext *ctx,
                                       float const p[3], int *outHit,
                                       int *outFace, float outUV[2],
                                       float outP[3], float outN[3]);

/* Graph-mode edits. *outId receives the new node/edge id. */
int USDGENTONIC_API Tonic_GraphAddNode(TonicModelContext *ctx, int faceId,
                                       float u, float v, int *outId);
/* Atomically closes one graph region. `nodeIds[i] >= 0` reuses that exact
 * live stable node id and ignores faceIds/uvs at i; `nodeIds[i] == -1`
 * creates a node at faceIds[i], uvs[2*i..2*i+1]. Existing chain edges are
 * reused. `outRegionId` may be NULL. */
int USDGENTONIC_API Tonic_GraphCreateRegion(
    TonicModelContext *ctx, int const *nodeIds, int const *faceIds,
    float const *uvs, int count, int *outRegionId);
int USDGENTONIC_API Tonic_GraphMoveNode(TonicModelContext *ctx, int nodeId,
                                        int faceId, float u, float v);
/* Atomically moves existing graph nodes to K1 face/UV locations. Used by a
 * whole-edge drag: all targets are checked before its endpoints move, K2
 * retraces once, and any topology-changing target is rejected unchanged. */
int USDGENTONIC_API Tonic_GraphMoveNodes(
    TonicModelContext *ctx, int const *nodeIds, int const *faceIds,
    float const *uvs, int count);
int USDGENTONIC_API Tonic_GraphConnect(TonicModelContext *ctx, int a, int b,
                                       int *outEdge);
int USDGENTONIC_API Tonic_GraphSplitEdge(TonicModelContext *ctx, int edgeId,
                                         int faceId, float u, float v,
                                         int *outNode);
int USDGENTONIC_API Tonic_GraphWeld(TonicModelContext *ctx, int keep,
                                    int drop);
int USDGENTONIC_API Tonic_GraphWeldAll(TonicModelContext *ctx, float radius,
                                       int *outWelds);
int USDGENTONIC_API Tonic_GraphUnweld(TonicModelContext *ctx, int nodeId,
                                      int *outIds, int maxOut, int *outCount);
int USDGENTONIC_API Tonic_GraphDeleteEdge(TonicModelContext *ctx, int edgeId);
int USDGENTONIC_API Tonic_GraphDeleteNode(TonicModelContext *ctx, int nodeId);
/* Snap queries. Returns the node/edge id, or -1 when nothing is in range
 * (a null context also returns -1). */
int USDGENTONIC_API Tonic_GraphSnapNode(TonicModelContext const *ctx,
                                        float const p[3], float radius);
int USDGENTONIC_API Tonic_GraphSnapEdge(TonicModelContext const *ctx,
                                        float const p[3], float radius);
/* Reads one exact stable graph node; returns TONIC_ERROR for dead/unknown
 * ids. All output pointers are required. */
int USDGENTONIC_API Tonic_GraphGetNode(TonicModelContext const *ctx,
                                       int nodeId, int *outFaceId,
                                       float outUV[2], float outP[3]);
/* Reads the stable endpoint node ids of one live stable edge id. */
int USDGENTONIC_API Tonic_GraphGetEdge(TonicModelContext const *ctx,
                                       int edgeId, int outNodeIds[2]);
/* Reads the viewport position of one stable graph node.  This is its
 * canonical scalp point plus the display-only normal lift used by the
 * scene index; GraphGetNode remains the authoring-coordinate getter. */
int USDGENTONIC_API Tonic_GraphGetNodeDisplayPosition(
    TonicModelContext const *ctx, int nodeId, float outP[3]);
int USDGENTONIC_API Tonic_GraphLinkRegions(TonicModelContext *ctx, int r0,
                                           int r1);
int USDGENTONIC_API Tonic_GraphUnlinkRegions(TonicModelContext *ctx, int r0,
                                             int r1);
/* A Draw stroke: `faceIds`/`uvs` (2 floats per sample) are one K1 hit per
 * mouse sample. Writes the node chain (up to maxOut ids) and the weld/loop
 * flags. */
int USDGENTONIC_API Tonic_GraphStroke(
    TonicModelContext *ctx, int const *faceIds, float const *uvs, int samples,
    float snapRadius, float simplifyEps, int *outNodes, int maxOut,
    int *outCount, int *outClosed, int *outWeldedStart, int *outWeldedEnd);
/* Mirror-X over the whole graph. Writes (oldId, newId) pairs, 2 ints each. */
int USDGENTONIC_API Tonic_GraphMirrorX(TonicModelContext *ctx, int *outPairs,
                                       int maxPairs, int *outCount);

int USDGENTONIC_API Tonic_SetSnapRadius(TonicModelContext *ctx, float radius);
float USDGENTONIC_API Tonic_GetSnapRadius(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_SetMirrorX(TonicModelContext *ctx, int on);
int USDGENTONIC_API Tonic_GetMirrorX(TonicModelContext const *ctx);

int USDGENTONIC_API Tonic_Rasterise(TonicModelContext *ctx);
/* Exact graph-region query at a K1 surface hit. Returns the source graph
 * region id (not the linked interpolation id), or -1 when the coordinate is
 * uncovered or invalid. This intentionally does not use the coarse per-face
 * K3 map: multiple closed regions may occupy one scalp face. */
int USDGENTONIC_API Tonic_RegionAtSurface(TonicModelContext const *ctx,
                                          int faceId, float u, float v);
unsigned long long USDGENTONIC_API Tonic_GetMapVersion(
    TonicModelContext const *ctx);

/* Census + HUD data: alive nodes/edges/regions, uncovered faces,
 * root-intersected faces. Any out-param may be NULL. */
int USDGENTONIC_API Tonic_GetGraphCounts(TonicModelContext const *ctx,
                                         int *outNodes, int *outEdges,
                                         int *outRegions);
int USDGENTONIC_API Tonic_GetRegionStats(TonicModelContext const *ctx,
                                         int *outRegions, int *outUncovered,
                                         int *outIntersected);
/* The live region map (per-face interp ids, -1 uncovered). `out` must hold
 * the scalp face count ints; *outCount receives it. */
int USDGENTONIC_API Tonic_ReadFaceRegions(TonicModelContext *ctx, int *out,
                                          int maxOut, int *outCount);
/* Per-face lowest claiming region id (-1 uncovered): link picking reads
 * this, not the interp ids. Same sizing rule as Tonic_ReadFaceRegions. */
int USDGENTONIC_API Tonic_ReadFaceRegionIds(TonicModelContext *ctx, int *out,
                                            int maxOut, int *outCount);
/* Graph census reads for the scene-index publisher and the HUD. */
int USDGENTONIC_API Tonic_ReadGraphNodes(TonicModelContext *ctx,
                                         int *outFaceIds, float *outUV,
                                         float *outP, int maxNodes,
                                         int *outCount);
int USDGENTONIC_API Tonic_ReadGraphEdges(TonicModelContext *ctx,
                                         int *outPairs, int maxEdges,
                                         int *outCount);
int USDGENTONIC_API Tonic_ReadRegionColors(TonicModelContext *ctx,
                                           float *outRGB, int maxRegions,
                                           int *outCount);
/* Region loops as stable node ids (polygon-style): `outCounts` takes the
 * per-region node count (maxRegions entries), `outIndices` the flattened
 * loops (maxIndices entries). Either array may be NULL to query its size. */
int USDGENTONIC_API Tonic_ReadRegionLoops(TonicModelContext *ctx,
                                          int *outCounts, int maxRegions,
                                          int *outIndices, int maxIndices,
                                          int *outRegionCount,
                                          int *outIndexCount);

/* P3 Tube mode (plan/17 section 5.2, K4/K5). `ringVerts=0` matches the
 * canonical region-loop CV count. Explicit 3..32 retains every authored
 * corner and adds only deterministic samples along region edges. Center CVs,
 * sections, soft
 * selection and display density. Guides are NOT refilled here: the move
 * is an edit + Tonic_RefillGuides(preview), the release ends with
 * Tonic_RefillGuides(1.0). */
int USDGENTONIC_API Tonic_BuildTubeFromRegion(TonicModelContext *ctx,
                                              int regionId, int centerCount,
                                              int ringVerts, float length);
int USDGENTONIC_API Tonic_MoveCenterCV(TonicModelContext *ctx, int cv,
                                       float dx, float dy, float dz);
int USDGENTONIC_API Tonic_InsertCenterCV(TonicModelContext *ctx, int atIndex);
int USDGENTONIC_API Tonic_DeleteCenterCV(TonicModelContext *ctx, int index);
int USDGENTONIC_API Tonic_SetTubeLength(TonicModelContext *ctx, float length);
int USDGENTONIC_API Tonic_MatchSurface(TonicModelContext *ctx);
/* Center census. Null context returns -1. */
int USDGENTONIC_API Tonic_GetCenterCVCount(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_GetCenterCV(TonicModelContext *ctx, int cv,
                                      float outXYZ[3]);
/* Section census + reads. Null context returns -1. Tonic_GetSection
 * writes the ring's t, its (u, v) pairs (2 * ringVerts floats), scale
 * and twist; *outCount receives the ring CV count. */
int USDGENTONIC_API Tonic_GetSectionCount(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_GetSection(TonicModelContext *ctx, int ring,
                                     float *outT, float *outUV, int uvLen,
                                     int *outCount, float *outScale,
                                     float *outTwist);
int USDGENTONIC_API Tonic_MoveSectionRing(TonicModelContext *ctx, int ring,
                                          float du, float dv);
int USDGENTONIC_API Tonic_ScaleSectionRing(TonicModelContext *ctx, int ring,
                                           float scale);
int USDGENTONIC_API Tonic_TwistSectionRing(TonicModelContext *ctx, int ring,
                                           float radians);
int USDGENTONIC_API Tonic_MoveSectionCV(TonicModelContext *ctx, int ring,
                                        int slot, float du, float dv);
/* *outRing receives the new ring count (inserted rings sort by t). */
int USDGENTONIC_API Tonic_AddSectionRing(TonicModelContext *ctx, float t,
                                         int *outRing);
int USDGENTONIC_API Tonic_RemoveSectionRing(TonicModelContext *ctx, int ring);
int USDGENTONIC_API Tonic_CopySectionRing(TonicModelContext *ctx, int src,
                                          int dst);
int USDGENTONIC_API Tonic_SetSoftSelection(TonicModelContext *ctx,
                                           float center, float radius);
int USDGENTONIC_API Tonic_GetSoftSelection(TonicModelContext *ctx,
                                           float *outCenter,
                                           float *outRadius);
int USDGENTONIC_API Tonic_RelaxCenter(TonicModelContext *ctx, float strength,
                                      int iterations);
int USDGENTONIC_API Tonic_SnapRootToScalp(TonicModelContext *ctx);
int USDGENTONIC_API Tonic_SetDisplaySegments(TonicModelContext *ctx,
                                             int segments);
int USDGENTONIC_API Tonic_GetDisplaySegments(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_SetTubeRegionId(TonicModelContext *ctx, int regionId);
int USDGENTONIC_API Tonic_GetTubeRegionId(TonicModelContext const *ctx);
/* Faces at the tube's interpolation id. `out` must hold maxOut ints;
 * *outCount receives the face count. */
int USDGENTONIC_API Tonic_ReadTubeRegionFaces(TonicModelContext *ctx, int *out,
                                              int maxOut, int *outCount);

/* P3 Fill mode (plan/17 section 5.3, K8-K10). Tonic_SetFillParams takes
 * the density, CV count, seed, edge bias and the flattened length-profile
 * (position, value) pairs (pairFloats must be even). */
int USDGENTONIC_API Tonic_SetFillParams(TonicModelContext *ctx, float density,
                                        int cvCount, int seed, float edgeBias,
                                        float const *profilePairs,
                                        int pairFloats);
int USDGENTONIC_API Tonic_GetFillParams(TonicModelContext *ctx,
                                        float *outDensity, int *outCvCount,
                                        int *outSeed, float *outEdgeBias,
                                        float *outProfile, int maxFloats,
                                        int *outFloats);
/* Commit-only output. `densityMultiplier` scales the authored full Fill
 * density (not the interactive preview fraction); width is in scene units.
 * `enabled` is exactly 0 or 1. Get requires all three output pointers. */
int USDGENTONIC_API Tonic_SetOutputSettings(TonicModelContext *ctx,
                                            int enabled,
                                            float densityMultiplier,
                                            float width);
int USDGENTONIC_API Tonic_GetOutputSettings(TonicModelContext const *ctx,
                                            int *outEnabled,
                                            float *outDensityMultiplier,
                                            float *outWidth);
int USDGENTONIC_API Tonic_SetPreviewFraction(TonicModelContext *ctx,
                                             float fraction);
float USDGENTONIC_API Tonic_GetPreviewFraction(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_SetFreezeRoots(TonicModelContext *ctx, int on);
int USDGENTONIC_API Tonic_GetFreezeRoots(TonicModelContext const *ctx);
/* Refill through K8/K9/K10; negative fraction selects the stored preview
 * fraction, 1.0 refills at full density. */
int USDGENTONIC_API Tonic_RefillGuides(TonicModelContext *ctx, float fraction);
/* Explicit Fill after Clear. Auto-refresh RefillGuides stays suppressed. */
int USDGENTONIC_API Tonic_GenerateGuides(TonicModelContext *ctx,
                                         float fraction);
int USDGENTONIC_API Tonic_ClearGeneratedCurves(TonicModelContext *ctx);
int USDGENTONIC_API Tonic_SetGeneratedCurvesVisible(TonicModelContext *ctx,
                                                     int visible);
int USDGENTONIC_API Tonic_GetGeneratedCurvesVisible(
    TonicModelContext const *ctx, int *outVisible);
/* Guide census. Any out-param may be NULL. */
int USDGENTONIC_API Tonic_GetGuideCounts(TonicModelContext const *ctx,
                                         int *outGuides, int *outCv);
/* Tubes the last refill skipped (they produced no guides), in fill order
 * (two-call (NULL, 0) probe). A refill still returns TONIC_OK while any
 * tube filled, so this is how a partial drop reaches the dock. */
int USDGENTONIC_API Tonic_ReadRefillDrops(TonicModelContext const *ctx,
                                          int *out, int outCap,
                                          int *outCount);
/* The reason the index-th dropped tube was skipped ("" when out of
 * range). The string stays valid until the next call on this thread. */
const char *USDGENTONIC_API Tonic_GetRefillDropReason(
    TonicModelContext const *ctx, int index);
/* The current guide set (guide-major points + per-guide counts).
 * `outXYZ` must hold 3 * guides * cv floats, `outCounts` guides ints;
 * *outGuideCount receives the guide count. */
int USDGENTONIC_API Tonic_ReadGuidePreview(TonicModelContext *ctx,
                                           float *outXYZ, int xyzLen,
                                           int *outCounts, int countsLen,
                                           int *outGuideCount);
/* Root census: face ids, world positions (3 per root) and normalised
 * root-plane coords (2 per root). Any array may be NULL to query only
 * *outCount. */
int USDGENTONIC_API Tonic_ReadGuideRoots(TonicModelContext *ctx,
                                         int *outFaceIds, float *outXYZ,
                                         float *outRU, int maxRoots,
                                         int *outCount);

/* P3 pick (plan/17 section 4.6, K11). Screen-space pick with a kind mask
 * (a TonicPickKind OR from tonicTube.h; TonicPick_All picks everything).
 * viewProj is row-major USD (the gpu/picking.cu convention), (x, y) is
 * top-left-origin pixels. *outHit is 1 on a hit (kind/index/subIndex +
 * distance/depth written) and 0 on a miss (outputs untouched). */
int USDGENTONIC_API Tonic_Pick(TonicModelContext *ctx,
                               float const viewProj[16], int w, int h, float x,
                               float y, float radiusPx, unsigned int kindMask,
                               int *outHit, unsigned int *outKind,
                               int *outIndex, int *outSubIndex,
                               float *outDistPx, float *outDepth);

/* P1 commit pipeline (plan/17 section 3). The committer owns a worker thread
 * that snapshots the model and builds anonymous SdfLayers; the UI thread
 * swaps them into the tool-owned live sublayer at idle. Layers cross as
 * identifiers (anonymous layers have them too), so this stays ctypes-safe:
 * no Sdf/Usd handle ever crosses. Returns TonicCommitter_Swap* codes. */
typedef struct TonicCommitterContext TonicCommitterContext;

#define TonicCommitter_Swapped 0
#define TonicCommitter_PartialProgress 1
#define TonicCommitter_NothingPending 2
#define TonicCommitter_SkippedGesture 3
#define TonicCommitter_SkippedStale 4
#define TonicCommitter_Detached 5
/* Tonic_CommitterSwap failed (null argument, unknown live layer, a throw);
 * the reason is in Tonic_GetLastError. Negative so it can never be read as
 * TonicCommitter_PartialProgress, which TONIC_ERROR (1) used to alias. */
#define TonicCommitter_Error (-1)

/* Create a committer for `modelCtx` (which must outlive it) writing the
 * groom at `groomPath` and filling the description at `descPath` (NULL or
 * empty disables the GuideInterpolate fill-in). */
int USDGENTONIC_API Tonic_CommitterCreate(
    TonicModelContext *modelCtx, const char *groomPath, const char *descPath,
    TonicCommitterContext **outCc);
int USDGENTONIC_API Tonic_CommitterDestroy(TonicCommitterContext *cc);

/* Enqueue a commit of the model's latest version. The GuideInterpolate
 * fill-in plan is computed by the caller over pxr (TonicPlanGuideInterpolate-
 * Fill documents the rule) and passed as flags; Enqueue() with all zeros
 * plans no fill-in for this version. `opPath` names the op the flags target
 * (existing or new); NULL selects the default. */
int USDGENTONIC_API Tonic_CommitterEnqueue(
    TonicCommitterContext *cc, int createOp, int setGuides, int setRegion,
    const char *opPath);

/* Swap the latest built layer into the live layer named `liveIdentifier`
 * (found via SdfLayer::Find). `gestureActive` nonzero holds the swap back.
 * Returns a TonicCommitter_Swap* code, or TonicCommitter_Error (with
 * GetLastError) when the live layer is unknown or the swap failed. */
int USDGENTONIC_API Tonic_CommitterSwap(TonicCommitterContext *cc,
                                        const char *liveIdentifier,
                                        int gestureActive);

/* Take the committer's last failure: a worker build that failed or threw,
 * or an enqueue the artist-owned-output guard refused. The text is cleared
 * by the call, so a tool pumping once per idle slot reports each failure
 * once. Copies at most cap-1 bytes plus a NUL into `out` and returns the
 * full length (0 when there is nothing to take; a return >= cap means the
 * copy was truncated). With out NULL or cap <= 0 the text is still taken
 * and only its length returned. -1 (with GetLastError) on a null
 * committer. */
int USDGENTONIC_API Tonic_CommitterTakeDiagnostic(TonicCommitterContext *cc,
                                                  char *out, int cap);

/* The model version whose build failed or whose enqueue was refused, or 0
 * when the latest enqueue has not failed. The worker never retries it on
 * its own; a pending version at or below it is not in flight, and only a
 * fresh Tonic_CommitterEnqueue tries again. */
unsigned long long USDGENTONIC_API Tonic_CommitterFailedVersion(
    TonicCommitterContext const *cc);

unsigned long long USDGENTONIC_API Tonic_CommitterCommittedVersion(
    TonicCommitterContext const *cc);
unsigned long long USDGENTONIC_API Tonic_CommitterPendingVersion(
    TonicCommitterContext const *cc);
double USDGENTONIC_API Tonic_CommitterLastSwapMs(
    TonicCommitterContext const *cc);
int USDGENTONIC_API Tonic_CommitterPartialMode(
    TonicCommitterContext const *cc);
int USDGENTONIC_API Tonic_CommitterSetSwapBudgetMs(TonicCommitterContext *cc,
                                                   double ms);

/* Abandon the usdGen cook the last swap started (plan/17 section 3.2 rule
 * 2, plan/18 section 7 G7). Called at gesture press: the cook in flight is
 * describing a groom the model has already left, and "show amplified hair"
 * has just hidden its tiles anyway. Returns the number of description
 * sessions whose cancellation token moved, or -1 on error. Safe with no
 * description linked (returns 0). */
int USDGENTONIC_API Tonic_CommitterCancelCooks(TonicCommitterContext *cc);

/* Retarget the scalp link (takes effect on the next build). NULL or empty
 * clears it. */
int USDGENTONIC_API Tonic_CommitterSetScalpPath(TonicCommitterContext *cc,
                                                const char *scalpPath);

/* Stage closed or reloaded under the tool (plan/17 section 3.4): Detach
 * idles the committer (the model survives); Swap reports Detached until
 * Reattach. Enqueues while detached are dropped; the tool enqueues fresh
 * after Reattach and the next swap re-hydrates (or re-creates) the groom
 * prim in the new live layer. */
int USDGENTONIC_API Tonic_CommitterDetach(TonicCommitterContext *cc);
int USDGENTONIC_API Tonic_CommitterReattach(TonicCommitterContext *cc);

/* P2 bake pipeline (plan/17 section 3.1a). The bake worker owns its thread
 * and its lowest-priority CUDA stream; the UI thread enqueues map versions
 * and swaps finished files with one attribute author each. */
typedef struct TonicBakeContext TonicBakeContext;

/* Create a bake worker for `modelCtx` (which must outlive it). Versioned
 * files land in `outDir` as `<baseName>.v<m>.ptx` (`baseName` NULL/empty
 * selects "regionMap"). */
int USDGENTONIC_API Tonic_BakeCreate(TonicModelContext *modelCtx,
                                     const char *outDir, const char *baseName,
                                     TonicBakeContext **outBake);
int USDGENTONIC_API Tonic_BakeDestroy(TonicBakeContext *bake);

/* Bake options: `resOverride` forces one per-face texel resolution (log2,
 * -1 selects auto), `levelCount` sets the baked channel count (P2: 1). */
int USDGENTONIC_API Tonic_BakeSetOptions(TonicBakeContext *bake,
                                         int resOverride, int levelCount);

/* Enqueue a bake of the model's current graph at its map version. */
int USDGENTONIC_API Tonic_BakeEnqueue(TonicBakeContext *bake);

/* Take the latest finished bake (UI thread, at idle). Returns 1 with
 * *outVersion + the NUL-terminated path when a bake completed since the
 * last call, else 0. */
int USDGENTONIC_API Tonic_BakeTakeCompleted(TonicBakeContext *bake,
                                            unsigned long long *outVersion,
                                            char *outPath, int pathLen);

/* Swap a finished bake into the live layer (UI thread, at idle): authors
 * exactly one attribute (usdGen:map:file on `regionMapPath`), records the
 * file on the model (so section 3.1 builds re-author it), and tells the
 * worker to sweep stale versions. */
int USDGENTONIC_API Tonic_BakeSwap(TonicBakeContext *bake,
                                   unsigned long long mapVersion,
                                   const char *liveIdentifier,
                                   const char *regionMapPath,
                                   const char *file);

unsigned long long USDGENTONIC_API Tonic_BakePendingVersion(
    TonicBakeContext const *bake);
unsigned long long USDGENTONIC_API Tonic_BakeCompletedVersion(
    TonicBakeContext const *bake);

/* -- P4: hierarchy + sculpt (plan/17 §2.4, §5.4/§5.5) -------------------- */

/* Split tubeId into count children (K14); outIds takes the child ids. */
int USDGENTONIC_API Tonic_SubdivideTube(TonicModelContext *ctx, int tubeId,
                                        int count, const char *splitMode,
                                        int seed, int *outIds, int outCap,
                                        int *outCount);
/* Merge tubeId's children back into it (K7 last pass + removal). */
int USDGENTONIC_API Tonic_MergeChildren(TonicModelContext *ctx, int tubeId);
/* Fold sibling tubeIds into one child; the kept id is tubeIds[0]. */
int USDGENTONIC_API Tonic_MergeSelected(TonicModelContext *ctx,
                                        int const *tubeIds, int idCount,
                                        int *outKept);
/* Live tube count / level of tubeId (-1 when missing). Direct returns. */
int USDGENTONIC_API Tonic_GetTubeCount(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_GetTubeLevel(TonicModelContext const *ctx,
                                       int tubeId);
/* Child ids of tubeId in ascending order. */
int USDGENTONIC_API Tonic_GetTubeChildren(TonicModelContext const *ctx,
                                          int tubeId, int *outIds, int outCap,
                                          int *outCount);
/* Center-CV count of tubeId (-1 when missing). Direct return. */
int USDGENTONIC_API Tonic_GetTubeCenterCount(TonicModelContext const *ctx,
                                             int tubeId);
/* Center CV into outXYZ[3]. */
int USDGENTONIC_API Tonic_GetTubeCenterCV(TonicModelContext const *ctx,
                                          int tubeId, int cv, float *outXYZ);
/* Visible/editable center-CV handle into outXYZ[3]. This is the actual
 * section polygon area centroid; Tonic_GetTubeCenterCV remains raw authored
 * center-cage data. */
int USDGENTONIC_API Tonic_GetTubeCenterHandle(TonicModelContext const *ctx,
                                              int tubeId, int cv,
                                              float *outXYZ);
/* Move one center CV with K6-down / K7-up propagation. */
int USDGENTONIC_API Tonic_MoveTubeCenterCV(TonicModelContext *ctx, int tubeId,
                                           int cv, float dx, float dy,
                                           float dz);
/* Translate every center CV in one tube atomically, then run K6/K7 once. */
int USDGENTONIC_API Tonic_TranslateTube(TonicModelContext *ctx, int tubeId,
                                        float dx, float dy, float dz);
/* Flattened center deltas (du, dv, dw per CV); two-call (NULL, 0) probe. */
int USDGENTONIC_API Tonic_ReadTubeDeltas(TonicModelContext const *ctx,
                                         int tubeId, float *out, int outCap,
                                         int *outCount);
/* Propagation gates; tubeId -1 = global default. */
int USDGENTONIC_API Tonic_SetLockParents(TonicModelContext *ctx, int tubeId,
                                         int on);
int USDGENTONIC_API Tonic_SetLockChildren(TonicModelContext *ctx, int tubeId,
                                          int on);
/* K7 on-the-fly parent over tubeIds; returns its (negative) id. */
int USDGENTONIC_API Tonic_GroupTubes(TonicModelContext *ctx,
                                     int const *tubeIds, int idCount,
                                     int makeTransient, int *outParent);
int USDGENTONIC_API Tonic_MakePersistent(TonicModelContext *ctx, int tubeId);
/* Sculpt stroke over center CVs (brush shapes arrive precomputed). */
int USDGENTONIC_API Tonic_SculptStroke(TonicModelContext *ctx, int tubeId,
                                       const char *brush, int const *cvIds,
                                       float const *deltas, int cvCount,
                                       int preserveLength, int mirrorX);
/* K13 per-CV kink scores of tube 0 (two-call (NULL, 0) probe). Empty when
 * no tube is built. */
int USDGENTONIC_API Tonic_ReadSmoothnessScores(TonicModelContext const *ctx,
                                               float *out, int outCap,
                                               int *outCount);
/* K12 root-overlap check over tube 0 plus the store (post-gesture). */
int USDGENTONIC_API Tonic_CheckRootIntersections(TonicModelContext *ctx);
/* Tube ids flagged by the last check, ascending (two-call probe). */
int USDGENTONIC_API Tonic_ReadIntersectedTubes(TonicModelContext const *ctx,
                                               int *out, int outCap,
                                               int *outCount);
/* -- P5 bridges (plan/17 section 5.7) -------------------------------------- */
/* All tube ids in the model, ascending (two-call (NULL, 0) probe). */
int USDGENTONIC_API Tonic_ReadTubeIds(TonicModelContext const *ctx, int *out,
                                      int outCap, int *outCount);
/* -- V6: one L1 tube per region (plan/18 section 7 G14) -------------------- */
/* The L1 root ids, ascending (two-call (NULL, 0) probe). Tube 0 is first
 * when it is built; every further region's tube follows. */
int USDGENTONIC_API Tonic_ReadL1TubeIds(TonicModelContext const *ctx, int *out,
                                        int outCap, int *outCount);
/* The tube rooted in regionId, or -1 when the region has no stub yet. */
int USDGENTONIC_API Tonic_TubeForRegion(TonicModelContext const *ctx,
                                        int regionId);
/* The region a tube (or its L1 ancestor) is rooted in, or -1. */
int USDGENTONIC_API Tonic_RegionForTube(TonicModelContext const *ctx,
                                        int tubeId);
/* Re-attach the L1 tubes to the regions a graph edit left behind. Rasterise
 * already calls this; the entry point exists for a tool that edits the graph
 * and rasterises separately. outRebuilt/outRemoved may be NULL. Returns
 * TONIC_ERROR with the section 5.1 "merge the children first" diagnostic when
 * a tube that would move or go away carries child deltas. */
int USDGENTONIC_API Tonic_SyncRegionTubes(TonicModelContext *ctx,
                                          int *outRebuilt, int *outRemoved);
/* Import explicit centers + sections as a locked child of parentId. secT
 * takes nSec ascending values in [0, 1]; secU/secV take nSec * ringVerts
 * row-major (scale/twist default to 1/0). *outTubeId receives the child
 * id. Returns TONIC_OK/TONIC_ERROR (use Tonic_GetLastError). */
int USDGENTONIC_API Tonic_ImportLockedTube(
    TonicModelContext *ctx, int parentId, float const *cx, float const *cy,
    float const *cz, int nCv, float const *secT, float const *secU,
    float const *secV, int nSec, int ringVerts, int *outTubeId);
/* Swept-mesh import: ring-major world points (ringCount * ringVerts, 3
 * floats each, no caps) as a locked child of parentId. *outTubeId
 * receives the child id. */
int USDGENTONIC_API Tonic_ImportSweptMesh(TonicModelContext *ctx, int parentId,
                                          float const *points, int ringCount,
                                          int ringVerts, int *outTubeId);
/* Section census + reads for any tube (tube 0 included). Count is -1
 * when the tube is unknown. Tonic_GetTubeSection mirrors Tonic_GetSection
 * (ring t, (u, v) pairs, scale, twist). */
int USDGENTONIC_API Tonic_GetTubeSectionCount(TonicModelContext const *ctx,
                                              int tubeId);
int USDGENTONIC_API Tonic_GetTubeSection(TonicModelContext *ctx, int tubeId,
                                         int ring, float *outT, float *outUV,
                                         int uvLen, int *outCount,
                                         float *outScale, float *outTwist);
/* 1 when the tube is a bridge import, else 0 (unknown tubes read 0). */
int USDGENTONIC_API Tonic_IsTubeImported(TonicModelContext const *ctx,
                                         int tubeId);

/* -- V0 viewport publication (plan/18 section 2.1, 2.2) ---------------------
 *
 * Tonic_Create registers its model with the process-global TonicRegistry;
 * the Tonic scene indices attach to the same registry. Tonic_Activate names
 * the one model the viewport draws, and Tonic_Publish is the ONLY way the
 * viewport learns about a change: the controller calls it after every
 * mutating entry point in a gesture, then asks the view to redraw. There is
 * no polling and no implicit publish, so a model the tool never publishes is
 * never on screen.
 *
 * Main thread only: publishing walks Hydra observers. */

/* The registry id of this model (> 0), or 0 when it is not registered. */
int USDGENTONIC_API Tonic_GetModelId(TonicModelContext const *ctx);
/* Make this model the one the viewport draws, and publish it. The static
 * test tube is removed on the first activation and never comes back. */
int USDGENTONIC_API Tonic_Activate(TonicModelContext *ctx);
/* Clear the active model (whatever it is) and publish the empty state. */
int USDGENTONIC_API Tonic_Deactivate(TonicModelContext *ctx);
/* Publish the active model to every attached scene index. `dirtyMask` is a
 * TonicDirty_* OR the caller wants republished on top of whatever the model
 * reports pending; 0 publishes exactly the model's own pending bits.
 * Returns the number of indices that published something, or -1 on error
 * (unlike the status-returning entries above — the count IS the result). */
int USDGENTONIC_API Tonic_Publish(TonicModelContext *ctx,
                                  unsigned int dirtyMask);

/* Per-level display (1-based). Visibility, x-ray and focus are viewport
 * state: they re-publish, never re-tessellate, and are not authored to USD.
 * `visible`/`xray` are 0/1. The focused level draws thick center curves and
 * large CV dots; level 0 focuses nothing. */
/* V6 (plan/18 section 2.4a): which tubes draw their cross-section rings and
 * ring CV dots. 0 = none, 1 = selected tubes only (the default), 2 = every
 * tube (Tube mode's Ring and Section sub-modes). */
int USDGENTONIC_API Tonic_SetRingDisplay(TonicModelContext *ctx, int mode);
int USDGENTONIC_API Tonic_GetRingDisplay(TonicModelContext const *ctx);
/* V6 (plan/17 section 3.2, plan/18 section 7 G7): "show amplified hair".
 * On, the usdGen cook's amplified tiles draw and the Tonic guide preview
 * hides; off, the preview draws and the tiles are hidden. A live gesture
 * hides the tiles either way and the index restores them on release. */
int USDGENTONIC_API Tonic_SetAmplifiedHair(TonicModelContext *ctx, int show);
int USDGENTONIC_API Tonic_GetAmplifiedHair(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_SetLevelDisplay(TonicModelContext *ctx, int level,
                                          int visible, int xray);
int USDGENTONIC_API Tonic_GetLevelDisplay(TonicModelContext const *ctx,
                                          int level, int *outVisible,
                                          int *outXray);
int USDGENTONIC_API Tonic_SetFocusLevel(TonicModelContext *ctx, int level);
int USDGENTONIC_API Tonic_GetFocusLevel(TonicModelContext const *ctx);
/* Per-branch hierarchy display cut. Disabled is the legacy behavior: every
 * live tube is visible and focus level controls the edit frontier. Enabled
 * starts with L1 roots collapsed; expanding a non-leaf hides that tube and
 * exposes its direct children without changing unrelated branches. */
int USDGENTONIC_API Tonic_SetActiveCutEnabled(TonicModelContext *ctx,
                                              int enabled);
int USDGENTONIC_API Tonic_GetActiveCutEnabled(TonicModelContext const *ctx);
int USDGENTONIC_API Tonic_SetTubeExpanded(TonicModelContext *ctx, int tubeId,
                                          int expanded);
int USDGENTONIC_API Tonic_GetTubeExpanded(TonicModelContext const *ctx,
                                          int tubeId);
int USDGENTONIC_API Tonic_IsTubeVisible(TonicModelContext const *ctx,
                                        int tubeId);
/* V8 (plan/18 section 2.4a): world units per screen pixel at the focus
 * point, so the overlay dots and curves can be a fixed number of pixels
 * across. Storm sizes points and curves in WORLD units, so without this
 * a CV dot is as big as the tube it sits on. The controller sets it when
 * it resolves a camera and whenever the frustum changes; 0 restores the
 * radius-relative sizing a model with no camera uses. */
int USDGENTONIC_API Tonic_SetDisplayScale(TonicModelContext *ctx,
                                          float worldPerPixel);
int USDGENTONIC_API Tonic_GetDisplayScale(TonicModelContext const *ctx,
                                          float *outWorldPerPixel);
/* V9 (plan/18 section 2.4a): apply THE display policy in one call.
 * `mode` and `subMode` are the ids tonicModes spells ("graph", "tube" +
 * "center"/"ring"/"section", "fill", "hierarchy", "sculpt", "output");
 * `focusLevel` is the level being edited, 0 for none. The call sets every
 * level's visibility, x-ray strength and centers flag, the ring display and
 * the focus level, so the viewport look of a mode is decided in exactly one
 * place (usdGenTonic::TonicPolicyLevelDisplay). An unknown mode is an error
 * and changes nothing. The per-level centers-only state the fallback ladder
 * owns is carried through untouched. */
int USDGENTONIC_API Tonic_SetDisplayPolicy(TonicModelContext *ctx,
                                           char const *mode,
                                           char const *subMode,
                                           int focusLevel);
/* The x-ray strength (0 when the level is opaque) and centers flag the
 * policy left on `level`. Null outputs are skipped. */
int USDGENTONIC_API Tonic_GetLevelDraw(TonicModelContext const *ctx,
                                       int level, float *outXrayOpacity,
                                       int *outCenters);
/* V9: the stage path of the groom this model commits to. While it is set
 * and the model is active, the scene index hides the committed
 * `<groom>/Guides` prim — last commit's curves, which would otherwise draw
 * plain white over this gesture's tubes. Hydra visibility only: the
 * amplifier reads the stage, so no cook is starved. "" hides nothing. */
int USDGENTONIC_API Tonic_SetGroomPath(TonicModelContext *ctx,
                                       char const *path);
int USDGENTONIC_API Tonic_GetGroomPath(TonicModelContext const *ctx,
                                       char *out, int cap);

/* What the attached scene indices actually published for `level`: the face,
 * point and tube counts of /__usdGenTonic/tubes/L<level>. TONIC_ERROR when
 * no attached index publishes that level (which is also how a headless
 * process reads: it has no index). Null outputs are skipped. */
int USDGENTONIC_API Tonic_GetPublishedLevelInfo(TonicModelContext const *ctx,
                                                int level, int *outFaceCount,
                                                int *outPointCount,
                                                int *outTubeCount);

/* -- V1: selection, overlays, gestures (plan/18 §2.3-§2.5) ----------------
 *
 * Selection is model state, so it survives a commit and every mode reads
 * one set. Kinds are the TonicPickKind bits from tonicTube.h — the same
 * enum the pick returns — and the ids are spelled per kind:
 *
 *   TonicPick_TubeVert     tube        id = tube id
 *   TonicPick_CenterCV     center CV   id = tube, subId = CV
 *   TonicPick_SectionCV    section CV  id = tube, subId = ring,
 *                                      subSubId = slot
 *   TonicPick_SectionRing  ring        id = tube, subId = ring
 *   TonicPick_GraphNode    node        id = node id
 *   TonicPick_GraphEdge    edge        id = edge id
 *   TonicPick_Region       region      id = region id
 *   TonicPick_Guide        guide       id = guide index
 *   TonicPick_Level        level       id = level (selects every tube at it)
 *
 * Unused sub-ids are passed as -1 (or the array as NULL). Selection is NOT
 * undoable: an artist who undoes a move wants the geometry back, not the
 * click before it.
 */

/* How a select call combines with what is already selected. */
#define TONIC_SELECT_SET     0  /* replace the kinds being set */
#define TONIC_SELECT_ADD     1  /* union */
#define TONIC_SELECT_TOGGLE  2  /* symmetric difference */

/* Drop every selected item whose kind is in `kindMask` (0 = all kinds).
 * The hover item clears with its own kind. */
int USDGENTONIC_API Tonic_SelectClear(TonicModelContext *ctx,
                                      unsigned int kindMask);
/* Select `n` items of one kind. Set replaces that kind, Add unions,
 * Toggle flips each. `subIds`/`subSubIds` may be NULL for kinds that do
 * not use them. */
int USDGENTONIC_API Tonic_SelectSet(TonicModelContext *ctx,
                                    unsigned int kind, const int *ids,
                                    const int *subIds, const int *subSubIds,
                                    int n);
int USDGENTONIC_API Tonic_SelectAdd(TonicModelContext *ctx,
                                    unsigned int kind, const int *ids,
                                    const int *subIds, const int *subSubIds,
                                    int n);
int USDGENTONIC_API Tonic_SelectToggle(TonicModelContext *ctx,
                                       unsigned int kind, const int *ids,
                                       const int *subIds,
                                       const int *subSubIds, int n);
/* Marquee: every candidate whose projection lands in the pixel rectangle,
 * over the same candidate sets and the same projection Tonic_Pick uses.
 * `mode` is one of TONIC_SELECT_*; an empty band in SET mode clears the
 * kinds it was asked for. */
int USDGENTONIC_API Tonic_SelectRect(TonicModelContext *ctx,
                                     const float *viewProj, int w, int h,
                                     float x0, float y0, float x1, float y1,
                                     unsigned int kindMask, int mode);
/* Lasso: the same, against a closed polygon of `n` (x, y) pixel pairs
 * (>= 3; the last point joins the first). */
int USDGENTONIC_API Tonic_SelectPolygon(TonicModelContext *ctx,
                                        const float *viewProj, int w, int h,
                                        const float *xy, int n,
                                        unsigned int kindMask, int mode);
/* The one hovered item. `kind` 0 clears it. */
int USDGENTONIC_API Tonic_SetHover(TonicModelContext *ctx, unsigned int kind,
                                   int id, int subId, int subSubId);
int USDGENTONIC_API Tonic_GetHover(TonicModelContext const *ctx,
                                   unsigned int *outKind, int *outId,
                                   int *outSubId, int *outSubSubId);
/* Read one kind back, ascending by (id, subId, subSubId). Any output array
 * may be NULL; *outCount always receives the true count, so a NULL-array
 * call sizes the buffers. TONIC_ERROR when cap is short for the arrays
 * that were passed. */
int USDGENTONIC_API Tonic_ReadSelection(TonicModelContext const *ctx,
                                        unsigned int kind, int *outIds,
                                        int *outSubIds, int *outSubSubIds,
                                        int cap, int *outCount);
/* How many items of the kinds in `kindMask` are selected (0 = all). */
int USDGENTONIC_API Tonic_GetSelectionCount(TonicModelContext const *ctx,
                                            unsigned int kindMask);
/* World bounds of the selection — where a gizmo goes, what Frame Selected
 * frames. TONIC_ERROR (outputs untouched) when nothing selected has a
 * position. */
int USDGENTONIC_API Tonic_GetSelectionBounds(TonicModelContext const *ctx,
                                             float *outMin, float *outMax);
/* Pick one pixel and report the SELECTION ITEM it names, translating the
 * candidate ordinals Tonic_Pick reports (a graph node's array slot) into
 * the stable ids the selection stores. This is what a press and a hover
 * sample call; the item then goes to Tonic_SelectSet or Tonic_SetHover.
 * *outHit is 1 on a hit and 0 on a miss (outputs untouched). */
int USDGENTONIC_API Tonic_PickItem(TonicModelContext *ctx,
                                   const float *viewProj, int w, int h,
                                   float x, float y, float radiusPx,
                                   unsigned int kindMask, int *outHit,
                                   unsigned int *outKind, int *outId,
                                   int *outSubId, int *outSubSubId);

/* The gizmo the viewport draws (plan/18 §2.4). `kind` is 0 none,
 * 1 translate, 2 ringTRS, 3 nodeTranslate, 4 rotate, 5 scale; `frame` is
 * the u, v, w axes, 3 floats each; `activeHandle` is -1 or a stable handle
 * id (0/1/2 axis, 3 legacy ring, 4 centre, 5/6/7 planar, 8 view, 9 free).
 * Setting the same record twice
 * dirties nothing.  Tonic_SetGizmo allows every handle; Tonic_SetGizmoEx
 * also takes `allowedMask`, bit (1 << handleId) set for each handle the
 * headless fallback may draw (0xFFFFFFFF = all), so it hides exactly what
 * the tool's own overlay hides (GZ-06). */
int USDGENTONIC_API Tonic_SetGizmo(TonicModelContext *ctx, int kind,
                                   const float *origin, const float *frame,
                                   float sizeWorld, int activeHandle);
int USDGENTONIC_API Tonic_SetGizmoEx(TonicModelContext *ctx, int kind,
                                     const float *origin, const float *frame,
                                     float sizeWorld, int activeHandle,
                                     unsigned int allowedMask);
int USDGENTONIC_API Tonic_GetGizmo(TonicModelContext const *ctx,
                                   int *outKind, float *outOrigin,
                                   float *outFrame, float *outSizeWorld,
                                   int *outActiveHandle);
int USDGENTONIC_API Tonic_GetGizmoAllowedMask(TonicModelContext const *ctx,
                                              unsigned int *outMask);
/* The sculpt brush ring. A radius <= 0 clears it. */
int USDGENTONIC_API Tonic_SetBrushRing(TonicModelContext *ctx,
                                       const float *center,
                                       const float *normal,
                                       float radiusWorld);
int USDGENTONIC_API Tonic_GetBrushRing(TonicModelContext const *ctx,
                                       int *outActive, float *outCenter,
                                       float *outNormal, float *outRadius);

/* The gesture bracket (plan/18 §3.2). Begin takes ONE undo snapshot and
 * suppresses the rest, so a 200-sample drag is one undo step instead of
 * 200; End seals it and bumps the version once; Cancel restores the
 * press-time base bit-exactly. A nested Begin is an error. Cancel writes
 * the dirty bits the caller must publish (the stage never saw the drag,
 * but the viewport did). */
int USDGENTONIC_API Tonic_BeginGesture(TonicModelContext *ctx,
                                       const char *label);
int USDGENTONIC_API Tonic_EndGesture(TonicModelContext *ctx);
int USDGENTONIC_API Tonic_CancelGesture(TonicModelContext *ctx,
                                        unsigned int *outDirty);
int USDGENTONIC_API Tonic_GetGestureDepth(TonicModelContext const *ctx);

/* Redo (plan/18 §2.5): the states undo left, cleared by any new mutation.
 * Like Tonic_Undo it writes the dirty bits to publish (NULL to ignore). */
int USDGENTONIC_API Tonic_Redo(TonicModelContext *ctx,
                               unsigned int *outDirty);
int USDGENTONIC_API Tonic_GetRedoDepth(TonicModelContext const *ctx);
/* The label of one history step, NUL-terminated, for the Edit strip.
 * `depth` 0 is the step Ctrl+Z would undo; NEGATIVE depths address the
 * redo stack (-1 is the step Ctrl+Y would redo). TONIC_ERROR when no such
 * step exists or the buffer is too small. */
int USDGENTONIC_API Tonic_GetUndoLabel(TonicModelContext const *ctx,
                                       int depth, char *out, int cap);

const char *USDGENTONIC_API Tonic_GetLastError(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* USDGEN_TONIC_API_C_ABI_H */
