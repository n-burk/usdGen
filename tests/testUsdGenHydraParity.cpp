// testUsdGenHydraParity — gate SI-12 (T1): BuildGraphDescFromHydra agrees
// exactly with BuildGraphDescFromStage on the G1–G4 fixtures (V2-11 item 4:
// node set, edges, structural digests) before the stage path is unplugged.
// RED until the Hydra builder is implemented; the stage builder is the oracle.
//
// Digests need no separate assertion: the Merkle structural digest is a pure
// function of the compared desc content, so field-exact equality implies
// digest equality. Node order compares exactly (S26 namespace tie-break must
// match); params compare as name-sorted sets (engine keys params by name).
//
// Requires the plugin path at runtime (adapters publish the Hydra values):
// run under ctest (ENVIRONMENT carries it) or sourced bin/_env.sh by hand.
// Fixture dir arrives as USDGEN_TEST_FIXTURE_DIR (absolute source path).

#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenGraphDescBuilderStage.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdGen;
using namespace usdGenImaging;

namespace {

int g_failures = 0;
int g_checks = 0;
int g_diffCap = 0;

void Check(bool ok, std::string const &what)
{
    ++g_checks;
    if (ok) {
        return;
    }
    ++g_failures;
    if (g_diffCap < 25) {
        ++g_diffCap;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

template <class T>
void CheckEq(T const &a, T const &b, std::string const &what)
{
    Check(a == b, what);
}

void CheckParams(std::vector<UsdGenParamValue> a,
                 std::vector<UsdGenParamValue> b, std::string const &ctx)
{
    auto byName = [](UsdGenParamValue const &x,
                     UsdGenParamValue const &y) {
        return x.name < y.name;
    };
    std::sort(a.begin(), a.end(), byName);
    std::sort(b.begin(), b.end(), byName);
    Check(a.size() == b.size(), ctx + " param count");
    size_t const n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        std::string const c = ctx + " param[" + a[i].name.GetString() + "]";
        Check(a[i].name == b[i].name, c + " name");
        Check(a[i].value == b[i].value, c + " value");
        Check(a[i].animated == b[i].animated, c + " animated");
    }
}

void CheckNode(UsdGenNodeDesc const &a, UsdGenNodeDesc const &b, size_t i)
{
    std::string const ctx =
        "node[" + std::to_string(i) + " " + a.path.GetString() + "]";
    CheckEq(a.path, b.path, ctx + " path");
    CheckEq(a.type, b.type, ctx + " type");
    CheckEq(a.mode, b.mode, ctx + " mode");
    CheckEq(a.algorithmVersion, b.algorithmVersion, ctx + " algorithmVersion");
    CheckEq(a.enabled, b.enabled, ctx + " enabled");
    CheckEq(a.seed, b.seed, ctx + " seed");
    CheckEq(a.blend, b.blend, ctx + " blend");
    CheckEq(a.space, b.space, ctx + " space");
    CheckEq(a.readPhase, b.readPhase, ctx + " readPhase");
    CheckEq(a.inputs, b.inputs, ctx + " inputs");
    CheckEq(a.references, b.references, ctx + " references");
    CheckEq(a.curves, b.curves, ctx + " curves");
    CheckEq(a.surfaces, b.surfaces, ctx + " surfaces");
    CheckEq(a.maps, b.maps, ctx + " maps");
    CheckParams(a.params, b.params, ctx);
    Check(a.ramps.size() == b.ramps.size(), ctx + " ramps count");
}

void CheckCurveSet(UsdGenCurveSetDesc const &a, UsdGenCurveSetDesc const &b,
                   size_t i)
{
    std::string const ctx =
        "curveSet[" + std::to_string(i) + " " + a.path.GetString() + "]";
    CheckEq(a.path, b.path, ctx + " path");
    CheckEq(a.role, b.role, ctx + " role");
    CheckEq(a.curveRole, b.curveRole, ctx + " curveRole");
    CheckEq(a.curveVertexCounts, b.curveVertexCounts, ctx + " counts");
    CheckEq(a.points, b.points, ctx + " points");
    CheckEq(a.rest, b.rest, ctx + " rest");
    CheckEq(a.widths, b.widths, ctx + " widths");
    CheckEq(a.type, b.type, ctx + " type");
    CheckEq(a.basis, b.basis, ctx + " basis");
    CheckEq(a.wrap, b.wrap, ctx + " wrap");
    CheckEq(a.widthsInterpolation, b.widthsInterpolation,
            ctx + " widthsInterpolation");
    CheckEq(a.skinPrim, b.skinPrim, ctx + " skinPrim");
    CheckEq(a.curveId, b.curveId, ctx + " curveId");
    CheckEq(a.skinPrimUv, b.skinPrimUv, ctx + " skinPrimUv");
    CheckEq(a.rootFrame, b.rootFrame, ctx + " rootFrame");
    CheckEq(a.frozenEpoch, b.frozenEpoch, ctx + " frozenEpoch");
    CheckEq(a.guideBlend, b.guideBlend, ctx + " guideBlend");
    CheckEq(a.curveGeneration, b.curveGeneration, ctx + " curveGeneration");
}

void CheckSurface(UsdGenSurfaceDesc const &a, UsdGenSurfaceDesc const &b,
                  size_t i)
{
    std::string const ctx =
        "surface[" + std::to_string(i) + " " + a.path.GetString() + "]";
    CheckEq(a.path, b.path, ctx + " path");
    CheckEq(a.faceVertexCounts, b.faceVertexCounts, ctx + " counts");
    CheckEq(a.faceVertexIndices, b.faceVertexIndices, ctx + " indices");
    CheckEq(a.restPoints, b.restPoints, ctx + " restPoints");
    CheckEq(a.points, b.points, ctx + " points");
    Check(a.samples.size() == b.samples.size(), ctx + " samples count");
    for (size_t s = 0; s < std::min(a.samples.size(), b.samples.size());
         ++s) {
        CheckEq(a.samples[s].time, b.samples[s].time, ctx + " sample time");
        CheckEq(a.samples[s].points, b.samples[s].points,
                ctx + " sample points");
    }
    CheckEq(a.velocities, b.velocities, ctx + " velocities");
    CheckEq(a.uv, b.uv, ctx + " uv");
    CheckEq(a.subsetFaces, b.subsetFaces, ctx + " subsetFaces");
    CheckEq(a.worldMatrix, b.worldMatrix, ctx + " worldMatrix");
    CheckEq(a.surfaceGeneration, b.surfaceGeneration,
            ctx + " surfaceGeneration");
}

void CheckMap(UsdGenMapDesc const &a, UsdGenMapDesc const &b, size_t i)
{
    std::string const ctx =
        "map[" + std::to_string(i) + " " + a.path.GetString() + "]";
    CheckEq(a.path, b.path, ctx + " path");
    CheckEq(a.type, b.type, ctx + " type");
    CheckEq(a.resolvedAssetPath, b.resolvedAssetPath, ctx + " asset");
    CheckEq(a.textureGeneration, b.textureGeneration, ctx + " generation");
    CheckParams(a.params, b.params, ctx);
}

int CheckDesc(UsdGenGraphDesc const &a, UsdGenGraphDesc const &b,
              std::string const &fixture)
{
    int const before = g_failures;
    std::string const ctx = "[" + fixture + "] ";
    CheckEq(a.description, b.description, ctx + "description");
    CheckEq(a.terminal, b.terminal, ctx + "terminal");
    CheckEq(a.time, b.time, ctx + "time");
    Check(a.nodes.size() == b.nodes.size(), ctx + "node count");
    for (size_t i = 0; i < std::min(a.nodes.size(), b.nodes.size()); ++i) {
        CheckNode(a.nodes[i], b.nodes[i], i);
    }
    Check(a.curveSets.size() == b.curveSets.size(), ctx + "curveSet count");
    for (size_t i = 0;
         i < std::min(a.curveSets.size(), b.curveSets.size()); ++i) {
        CheckCurveSet(a.curveSets[i], b.curveSets[i], i);
    }
    Check(a.surfaces.size() == b.surfaces.size(), ctx + "surface count");
    for (size_t i = 0;
         i < std::min(a.surfaces.size(), b.surfaces.size()); ++i) {
        CheckSurface(a.surfaces[i], b.surfaces[i], i);
    }
    Check(a.maps.size() == b.maps.size(), ctx + "map count");
    for (size_t i = 0; i < std::min(a.maps.size(), b.maps.size()); ++i) {
        CheckMap(a.maps[i], b.maps[i], i);
    }
    CheckEq(a.look.rootColor, b.look.rootColor, ctx + "look.rootColor");
    CheckEq(a.look.tipColor, b.look.tipColor, ctx + "look.tipColor");
    CheckEq(a.look.rampColors, b.look.rampColors, ctx + "look.rampColors");
    CheckEq(a.look.rampPositions, b.look.rampPositions,
            ctx + "look.rampPositions");
    CheckEq(a.look.rampInterpolation, b.look.rampInterpolation,
            ctx + "look.rampInterpolation");
    CheckEq(a.look.rampExponent, b.look.rampExponent,
            ctx + "look.rampExponent");
    CheckEq(a.look.bakeMode, b.look.bakeMode, ctx + "look.bakeMode");
    CheckEq(a.look.bakeTarget, b.look.bakeTarget, ctx + "look.bakeTarget");
    CheckEq(a.look.bakePrimvar, b.look.bakePrimvar, ctx + "look.bakePrimvar");
    CheckEq(a.look.hueJitter, b.look.hueJitter, ctx + "look.hueJitter");
    CheckEq(a.look.valueJitter, b.look.valueJitter, ctx + "look.valueJitter");
    CheckEq(a.look.jitterSeed, b.look.jitterSeed, ctx + "look.jitterSeed");
    CheckEq(a.densityScale, b.densityScale, ctx + "densityScale");
    CheckEq(a.defaultWidth, b.defaultWidth, ctx + "defaultWidth");
    CheckEq(a.renderDensityScale, b.renderDensityScale,
            ctx + "renderDensityScale");
    CheckEq(a.tileTarget, b.tileTarget, ctx + "tileTarget");
    CheckEq(a.curveBasis, b.curveBasis, ctx + "curveBasis");
    CheckEq(a.motionMode, b.motionMode, ctx + "motionMode");
    CheckEq(a.motionSampleCount, b.motionSampleCount,
            ctx + "motionSampleCount");
    CheckEq(a.pickTarget, b.pickTarget, ctx + "pickTarget");
    CheckEq(a.purpose, b.purpose, ctx + "purpose");
    CheckEq(a.visibility, b.visibility, ctx + "visibility");
    CheckEq(a.materialPath, b.materialPath, ctx + "materialPath");
    CheckEq(a.xformMatrix, b.xformMatrix, ctx + "xformMatrix");
    CheckEq(a.schemaVersion, b.schemaVersion, ctx + "schemaVersion");
    return g_failures - before;
}

}  // namespace

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char const *kFixtures[] = {
        "g1_lash4k.usda",
        "g2_brow40k.usda",
        "g3_head100k.usda",
        "g4_fur200k.usda",
    };
    int totalNew = 0;
    for (char const *name : kFixtures) {
        g_diffCap = 0;
        std::string const path =
            std::string(USDGEN_TEST_FIXTURE_DIR) + "/" + name;
        UsdStageRefPtr stage = UsdStage::Open(path);
        if (!stage) {
            std::printf("FAIL: cannot open %s\n", path.c_str());
            ++g_failures;
            continue;
        }
        SdfPath descPath;
        for (UsdPrim const &child :
             stage->GetDefaultPrim().GetChildren()) {
            if (child.GetPrimTypeInfo().GetTypeName() ==
                TfToken("UsdGenDescription")) {
                descPath = child.GetPath();
                break;
            }
        }
        if (descPath.IsEmpty()) {
            std::printf("FAIL: no UsdGenDescription in %s\n", name);
            ++g_failures;
            continue;
        }
        UsdGenGraphDescBuildOptions opts;
        opts.time = 0.0;
        UsdGenGraphDesc const fromStage =
            BuildGraphDescFromStage(stage, descPath, opts);
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        UsdImagingSceneIndices const sis =
            UsdImagingCreateSceneIndices(info);
        if (!sis.finalSceneIndex) {
            std::printf("FAIL: no final scene index for %s\n", name);
            ++g_failures;
            continue;
        }
        UsdGenGraphDesc const fromHydra =
            BuildGraphDescFromHydra(*sis.finalSceneIndex, descPath, opts);
        int const n = CheckDesc(fromStage, fromHydra, name);
        totalNew += n;
        std::printf("%s: %s (%d new diffs)\n", name,
                    n == 0 ? "PARITY" : "MISMATCH", n);
    }
    std::printf("hydraParity: %d checks, %d failures\n", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
