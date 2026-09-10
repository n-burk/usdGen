// testUsdGenTileContract — gate SI-1 / C2 (plan 06 §4.1, plan 05 §"SI-1",
// docs/m1/interfaces.md §7): the tile publication contract.
//
// Tier T0: engine + the two pure statics of UsdGenTilePublisher. No stage, no
// plugin load, no Hydra engine (the engine never sees a stage, S8).
//
// A 32768-curve groom (guides curve set -> UsdGenGrow terminal) is committed
// through the real session; every assertion is made on the PUBLISHED
// UsdGenTilePublication values and on the data sources the publisher builds
// from them (the identical payload Hydra's GetPrim would expose):
//   * SI-1: points.size() == sum(curveVertexCounts) per tile,
//           uniform arrays sized curveCount, vertex arrays sized total,
//   * tile count == min(target, chunks) and paths under
//     <description>/__usdGenRender/tile_NNNN (4-digit),
//   * HdType basisCurves; the C2 child-name set; cubic/pinned never authored,
//   * hand-authored per-tile fields (xform, purpose, visibility, material,
//     primOrigin) match the graph desc post-flattening,
//   * commit determinism (IsIdentical between identical commits),
//   * generation id strictly monotonic across commits,
//   * NoticesFor(UsdGenTileDirty) emits EXACTLY the §4.1 locator mapping.
//
// Timing-soft (plan 09): nothing here is budget-gated.

#include "usdGen/curveBuffer.h"
#include "usdGen/graphDesc.h"
#include "usdGen/opRegistry.h"
#include "usdGen/session.h"
#include "usdGen/types.h"

#include "usdGenImaging/usdGenTilePublisher.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdGen;
using namespace usdGenImaging;

namespace {

int g_failures = 0;
void Check(bool ok, std::string const &what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

const int kCurveCount = 32768;   // >= 32768 per plan 05's SI-1 fixture size
const int kVertsPerCurve = 4;

// ---------------------------------------------------------------------------
// Synthetic desc: 32768 straight 2-point guide curves feeding one UsdGenGrow.
// ---------------------------------------------------------------------------
UsdGenGraphDesc MakeGroomDesc(SdfPath const &descPath, float width = 0.02f)
{
    UsdGenGraphDesc d;
    d.description = descPath;
    d.terminal = descPath.AppendChild(TfToken("grow"));
    d.tileTarget = 64;
    d.curveBasis = TfToken("bspline");
    d.purpose = TfToken("render");
    d.visibility = TfToken("invisible");
    d.materialPath = descPath.AppendChild(TfToken("look")).AppendChild(TfToken("material"));
    d.pickTarget = TfToken("description");
    d.time = 0.0;

    UsdGenCurveSetDesc guides;
    guides.path = descPath.AppendChild(TfToken("guides"));
    guides.role = UsdGenRole::Reference;
    guides.curveRole = TfToken("guide");
    guides.curveVertexCounts = VtIntArray(kCurveCount, kVertsPerCurve);
    guides.points.resize(size_t(kCurveCount) * kVertsPerCurve);
    guides.curveId.resize(size_t(kCurveCount));
    for (int c = 0; c < kCurveCount; ++c) {
        // Deterministic lattice roots; z grows along the curve.
        float const u = float(c % 256) * 0.01f;
        float const v = float(c / 256) * 0.01f;
        for (int j = 0; j < kVertsPerCurve; ++j) {
            guides.points[size_t(c) * kVertsPerCurve + j] =
                GfVec3f(u, v, 0.25f * j);
        }
        guides.curveId[c] = uint64_t(c) + 1;
    }
    guides.widths = VtFloatArray(size_t(kCurveCount) * kVertsPerCurve, 1.0f);
    d.curveSets.push_back(std::move(guides));

    UsdGenNodeDesc grow;
    grow.path = descPath.AppendChild(TfToken("grow"));
    grow.type = TfToken("UsdGenGrow");
    grow.enabled = true;
    grow.blend = 1.0f;
    grow.seed = 7;
    grow.curves = { guides.path };
    grow.params.push_back(UsdGenParamValue{TfToken("width"), VtValue(width), false});
    d.nodes.push_back(std::move(grow));
    return d;
}

std::string TilePathRegexCheck(SdfPath const &descPath, UsdGenTileId tile)
{
    // Publisher's own statics are the contract's source of truth; assert the
    // documented shape too (plan 06 §4.1): description/__usdGenRender/tile_NNNN.
    SdfPath expected = UsdGenTilePublisher::TilePath(descPath, tile);
    std::string s = expected.GetText();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s/%s/tile_%04u", descPath.GetText(),
                  UsdGenTilePublisher::RenderNamespace().GetText(),
                  unsigned(tile));
    bool prefix = (s == buf);
    return prefix ? std::string() : ("path is " + s + ", expected " + buf);
}

std::set<std::string> ChildNames(HdContainerDataSource &c)
{
    std::set<std::string> names;
    for (TfToken const &t : c.GetNames()) names.insert(t.GetString());
    return names;
}

size_t SumCounts(VtIntArray const &counts)
{
    size_t n = 0;
    for (int i : counts) n += size_t(i);
    return n;
}

}  // namespace

int main()
{
    usdGenRegisterM1Operators();
    UsdGenTilePublisher::Register();      // idempotent; ensures tokens live

    SdfPath const descPath("/groom");
    UsdGenSession session;
    session.SetGraphDesc(MakeGroomDesc(descPath));
    UsdGenGenerationConstPtr gen =
        session.Commit(0.0, UsdGenCommitReason::LiveOverride);
    Check(bool(gen), "first commit publishes a generation");
    if (!gen) {
        std::printf("testUsdGenTileContract: FAILED (no generation)\n");
        return 1;
    }

    // ---- SI-1 / C2 per-tile value contract ------------------------------
    size_t totalCurves = 0, totalPoints = 0;
    bool si1 = true, uniformSizes = true, vertexSizes = true, bases = true,
         refines = true, extents = true, paths = true, xforms = true,
         inherit = true, idArrays = true;
    size_t countMismatch = 0;
    for (auto const &pub : gen->tiles) {
        size_t const curves = size_t(pub.curveVertexCounts.size());
        size_t const pts = SumCounts(pub.curveVertexCounts);
        totalCurves += curves;
        totalPoints += pub.points.size();
        if (pub.points.size() != pts) { si1 = false; ++countMismatch; }
        if (pub.hairT.size() != pts) vertexSizes = false;
        if (pub.hairId.size() != curves) uniformSizes = false;
        if (pub.st.size() != curves && !pub.st.empty()) uniformSizes = false;
        if (!pub.widths.empty() &&
            pub.widths.size() != curves && pub.widths.size() != pts)
            si1 = false;
        if (pub.displayColor.size() != curves && pub.displayColor.size() != pts)
            si1 = false;
        for (auto const &p : pub.extraUniform)
            if (p.interpolation == TfToken("uniform") && !p.f.empty() &&
                p.f.size() != curves) uniformSizes = false;
        if (pub.basis != "bspline") bases = false;
        if (pub.refineLevel != 2) refines = false;
        if (!(pub.extentMin[0] <= pub.extentMax[0] &&
              pub.extentMin[1] <= pub.extentMax[1] &&
              pub.extentMin[2] <= pub.extentMax[2])) extents = false;
        GfVec3d mn(+1e30, +1e30, +1e30), mx(-1e30, -1e30, -1e30);
        for (auto const &p : pub.points) {
            mn[0] = std::min(mn[0], double(p[0])); mn[1] = std::min(mn[1], double(p[1]));
            mn[2] = std::min(mn[2], double(p[2]));
            mx[0] = std::max(mx[0], double(p[0])); mx[1] = std::max(mx[1], double(p[1]));
            mx[2] = std::max(mx[2], double(p[2]));
        }
        if ((mn - pub.extentMin).GetLength() > 1e-4 ||
            (mx - pub.extentMax).GetLength() > 1e-4) extents = false;
        if (!TilePathRegexCheck(descPath, pub.tile).empty()) paths = false;
        if (!(pub.xformMatrix == GfMatrix4d(1.0))) xforms = false;
        if (pub.purpose != TfToken("render") || pub.visibility != TfToken("invisible") ||
            pub.materialPath != descPath.AppendChild(TfToken("look")).AppendChild(TfToken("material")))
            inherit = false;
        if (pub.primOrigin != descPath) inherit = false;   // pickTarget == description
    }
    Check(!gen->tiles.empty(), "published at least one tile");
    Check(totalCurves >= size_t(kCurveCount),
          "tile curve set covers >= 32768 curves (got " + std::to_string(totalCurves) + ")");
    Check(gen->tiles.size() <= size_t(64),
          "tile count <= tileTarget 64 (got " + std::to_string(gen->tiles.size()) + ")");
    Check(si1, "SI-1: points.size() == sum(curveVertexCounts) on every tile"
               " (mismatches: " + std::to_string(countMismatch) + ")");
    Check(uniformSizes, "uniform primvars sized curveCount on every tile");
    Check(vertexSizes, "vertex primvars sized points on every tile");
    Check(bases, "curve basis == usdGen:curve:basis (bspline) on every tile");
    Check(refines, "refineLevel == 2 (S-9 tier table) on every tile");
    Check(extents, "extent/min|max == min|max over tile points (exact per frame)");
    Check(paths, "tile primPaths == <desc>/__usdGenRender/tile_NNNN (4-digit)");
    Check(xforms, "identity xform matrix (rest-only graph)");
    Check(inherit, "purpose/visibility/material/primOrigin inherited by hand (C2)");

    // ---- Publisher-built data sources: HdType + C2 child-name set -------
    if (!gen->tiles.empty()) {
        UsdGenTilePublication const &pub = gen->tiles[0];
        HdDataSourceBaseHandle ds = UsdGenTilePublisher::BuildTileDataSource(pub);
        Check(bool(ds), "BuildTileDataSource returns a container");
        if (ds) {
            Check(ds->GetHdType() == HdType(HfToken("basisCurves")),
                  "tile prim HdType == basisCurves (got " +
                      ds->GetHdType().GetString() + ")");
            if (HdContainerDataSourceHandle c = HdContainerDataSource::Cast(ds)) {
                std::set<std::string> names = ChildNames(*c);
                for (char const *req : { "primvars", "extent", "xform", "type", "wrap",
                                         "refineLevel", "visibility", "purpose",
                                         "materialBind" }) {
                    Check(names.count(req) != 0,
                          std::string("C2 child '") + req + "' published");
                }
                if (HdContainerDataSourceHandle primvars =
                        HdContainerDataSource::Extract(*c, HdTokens->primvars)) {
                    std::set<std::string> pv = ChildNames(*primvars);
                    Check(pv.count("usdGenId") == 0,
                          "usdGenId is NOT published in M1 (S30 reserved)");
                    for (char const *req : { "points", "widths", "hairT", "hairId", "st",
                                             "displayColor" }) {
                        Check(pv.count(req) != 0,
                              std::string("primvar '") + req + "' published");
                    }
                }
                // type/wrap are contract constants, never authored.
                if (HdValueDataSourceHandle typeDs =
                        HdValueDataSource::Extract(*c, HfToken("type"))) {
                    GfToken typeVal;
                    if (typeDs->Get(HdType::Token(), &typeVal, 1)) {
                        Check(GfToken(typeVal) == GfToken(TfToken("cubic")),
                              "topology type == cubic (constant)");
                    }
                }
                if (HdValueDataSourceHandle wrapDs =
                        HdValueDataSource::Extract(*c, HfToken("wrap"))) {
                    GfToken wrapVal;
                    if (wrapDs->Get(HdType::Token(), &wrapVal, 1)) {
                        Check(GfToken(wrapVal) == GfToken(TfToken("pinned")),
                              "topology wrap == pinned (constant)");
                    }
                }
            }
        }
    }

    // ---- Determinism: identical inputs -> identical published values -----
    session.SetGraphDesc(MakeGroomDesc(descPath));
    UsdGenGenerationConstPtr gen2 =
        session.Commit(0.0, UsdGenCommitReason::LiveOverride);
    Check(bool(gen2), "second commit publishes");
    if (gen2) {
        Check(gen2->id > gen->id,
              "generation id strictly increases (" + std::to_string(gen->id) +
                  " -> " + std::to_string(gen2->id) + ")");
        bool identical = gen2->tiles.size() == gen->tiles.size();
        bool dsIdentical = true;
        if (identical) {
            for (size_t i = 0; i < gen->tiles.size() && identical; ++i) {
                UsdGenTilePublication const &a = gen->tiles[i];
                UsdGenTilePublication const &b = gen2->tiles[i];
                identical = a.tile == b.tile && a.primPath == b.primPath &&
                    a.points == b.points && a.curveVertexCounts == b.curveVertexCounts;
                if (HdDataSourceBaseHandle da = UsdGenTilePublisher::BuildTileDataSource(a),
                    db = UsdGenTilePublisher::BuildTileDataSource(b)) {
                    if (!HdDataSourceIsIdentical(HdDataSourceBaseHandle(da),
                                                 HdDataSourceBaseHandle(db)))
                        dsIdentical = false;
                }
            }
        }
        Check(identical, "re-commit of identical desc publishes identical tile payloads");
        Check(dsIdentical, "re-commit data sources compare HdDataSourceIsIdentical");
    }

    if (gen2 && !gen2->tiles.empty() && !gen->tiles.empty()) {
        // Additive overload (06 §4.1): the container can carry a prim-level
        // `generation` int source with the snapshot id.
        if (HdContainerDataSourceHandle ca =
                UsdGenTilePublisher::BuildTileDataSource(gen->tiles[0]),
            cb = UsdGenTilePublisher::BuildTileDataSource(
                gen2->tiles[0], int64_t(gen2->id));
            ca && cb) {
            Check(ca->Get(TfToken("generation")) == nullptr,
                  "no generation source without the stamp overload");
            HdPrimvarAttributeHandle stamp = HdPrimvarAttribute(
                cb->Get(TfToken("generation")));
            HdInt64AttributeHandle stampVal = HdInt64Attribute(
                stamp ? HdPrimvarAttributeGet(stamp) : nullptr);
            VtValue sv;
            if (stampVal) sv = stampVal->GetValue();
            Check(sv.Get<int64_t>() == int64_t(gen2->id),
                  "stamped generation source carries the snapshot id");
        }

    // ---- Dirty