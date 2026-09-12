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
#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/graphDesc.h"
#include "usdGen/opRegistry.h"
#include "usdGen/session.h"
#include "usdGen/types.h"

#include "usdGenImaging/usdGenTilePublisher.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
// Synthetic desc: a surface-scatter generator feeding one UsdGenGrow, the
// canonical M1 groom shape. A generator is required for tiles to exist at
// all: the scheduler only (re)partitions the tile set when a generator's
// capture yields curves (scheduler.cpp "Generators establish/refresh the
// topology"), and the M1 reference lane does not materialize curveSets into
// buffers (scheduler.cpp header: 03 §1.5). ~32 940 roots come out of a
// 18.15x18.15 mesh at the default density 100, >= the 32 768 SI-1 fixture
// size; guides ride along as a declared Reference curveSet (R23).

// ---------------------------------------------------------------------------
UsdGenGraphDesc MakeGroomDesc(SdfPath const &descPath, float width = 0.02f)
{
    UsdGenGraphDesc d;
    d.description = descPath;
    d.terminal = descPath.AppendChild(TfToken("grow"));
    d.tileTarget = 64;
    d.curveBasis = TfToken("bspline");
    d.xformMatrix = GfMatrix4d(1.0);   // stage-side: post-flattening desc matrix
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

    // Generator surface: a flat 4x4-corner grid scaled so rest area is
    // 18.15^2 = 329.42; at the default density 100 scatter emits ~32 940
    // roots (>= kCurveCount), spread over 9 faces.
    UsdGenSurfaceDesc surf;
    surf.path = descPath.AppendChild(TfToken("surface"));
    surf.id = 0;
    surf.faceVertexCounts = VtIntArray(9, 4);
    surf.faceVertexIndices = VtIntArray{0, 1, 5, 4, 1, 2, 6, 5, 2, 3, 7, 6,
                                        4, 5, 9, 8, 5, 6, 10, 9, 6, 7, 11, 10,
                                        8, 9, 13, 12, 9, 10, 14, 13, 10, 11, 15, 14};
    surf.restPoints.resize(16);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            surf.restPoints[size_t(j) * 4 + i] =
                GfVec3f(6.05f * i, 6.05f * j, 0.0f);
    d.surfaces.push_back(std::move(surf));

    UsdGenNodeDesc scatter;
    scatter.path = descPath.AppendChild(TfToken("scatter"));
    scatter.type = TfToken("UsdGenScatter");
    scatter.enabled = true;
    scatter.blend = 1.0f;
    scatter.seed = 11;
    scatter.surfaces.push_back(surf.path);
    scatter.params.push_back(UsdGenParamValue{TfToken("flip"), VtValue(false), false});
    d.nodes.push_back(std::move(scatter));

    UsdGenNodeDesc grow;
    grow.path = descPath.AppendChild(TfToken("grow"));
    grow.type = TfToken("UsdGenGrow");
    grow.enabled = true;
    grow.blend = 1.0f;
    grow.seed = 7;
    grow.inputs = { descPath.AppendChild(TfToken("scatter")) };   // scatter's roots
    grow.params.push_back(UsdGenParamValue{TfToken("segments"), VtValue(8), false});
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

std::set<std::string> ChildNames(HdContainerDataSourceHandle const &c)
{
    std::set<std::string> names;
    if (!c) return names;
    for (TfToken const &t : c->GetNames()) names.insert(t.GetString());
    return names;
}

// 26.08 has no HdDataSourceIsIdentical: compare leaf VtValues recursively.
bool ValuesIdentical(HdDataSourceBaseHandle const &a, HdDataSourceBaseHandle const &b)
{
    HdContainerDataSourceHandle ca = HdContainerDataSource::Cast(a);
    HdContainerDataSourceHandle cb = HdContainerDataSource::Cast(b);
    if (ca || cb) {
        if (!ca || !cb) return false;
        if (ca->GetNames() != cb->GetNames()) return false;
        for (TfToken const &t : ca->GetNames())
            if (!ValuesIdentical(ca->Get(t), cb->Get(t))) return false;
        return true;
    }
    HdSampledDataSourceHandle sa = HdSampledDataSource::Cast(a);
    HdSampledDataSourceHandle sb = HdSampledDataSource::Cast(b);
    if (sa && sb) return sa->GetValue(0.0) == sb->GetValue(0.0);
    return bool(sa) == bool(sb) && !sa;
}

// Leaf sample value at Default() time, or empty VtValue.
VtValue LeafValue(HdDataSourceBaseHandle ds)
{
    if (HdSampledDataSourceHandle s = HdSampledDataSource::Cast(ds))
        return s->GetValue(0.0);
    return VtValue();
}

// Token-valued leaf, tolerant of TfToken vs std::string storage.
bool LeafIsToken(VtValue const &v, char const *text)
{
    if (v.IsHolding<TfToken>()) return v.UncheckedGet<TfToken>() == TfToken(text);
    if (v.IsHolding<std::string>()) return v.UncheckedGet<std::string>() == text;
    return false;
}

// Int-valued leaf, tolerant of int/int64 storage.
bool LeafIsInt(VtValue const &v, int64_t expect)
{
    if (v.IsHolding<int>()) return int64_t(v.Get<int>()) == expect;
    if (v.IsHolding<int64_t>()) return v.Get<int64_t>() == expect;
    return false;
}

size_t SumCounts(VtIntArray const &counts)
{
    size_t n = 0;
    for (int i : counts) n += size_t(i);
    return n;
}

void CheckPartitionBoundaries()
{
    UsdGenGraphDesc desc = MakeGroomDesc(SdfPath("/partition"));
    desc.tileTarget = 32;
    UsdGenCompiler compiler;
    UsdGenGraph graph;
    UsdGenCompileResult result = compiler.Compile(desc, &graph);
    Check(result.ok, "boundary partition fixture compiles");
    if (!result.ok) return;
    for (int nChunks : {33, 34, 65}) {
        Check(graph.Repartition(nChunks * kUsdGenDefaultChunkSize, 1),
              "boundary repartition reports layout");
        auto tiles = graph.Tiles();
        Check(tiles.size() == 32, "small partition clamps to 32 tiles");
        uint32_t covered = 0;
        for (size_t i = 0; i < tiles.size(); ++i) {
            Check(tiles[i].firstChunk == covered && tiles[i].chunkCount >= 1 &&
                  tiles[i].chunkCount <= 3, "tile boundary has no phantom chunk");
            covered += tiles[i].chunkCount;
        }
        Check(covered == static_cast<uint32_t>(nChunks),
              "tile boundaries cover every chunk exactly once");
        if (nChunks == 33) {
            Check(tiles[0].chunkCount == 2 && tiles[1].chunkCount == 1 &&
                  tiles[31].chunkCount == 1, "33 chunks use [2,31x1]");
        } else if (nChunks == 34) {
            Check(tiles[0].chunkCount == 2 && tiles[1].chunkCount == 2 &&
                  tiles[2].chunkCount == 1 && tiles[31].chunkCount == 1,
                  "34 chunks use [2,2,30x1]");
        } else {
            bool golden = tiles[16].chunkCount == 2;
            for (int i = 0; i < 16; ++i) golden &= tiles[i].chunkCount == 3;
            for (int i = 17; i < 32; ++i) golden &= tiles[i].chunkCount == 1;
            Check(golden, "65 chunks use [16x3,2,15x1]");
        }
        for (int node = 0; node < graph.NodeCount(); ++node) {
            auto chunks = graph.Chunks(static_cast<UsdGenGraph::NodeId>(node));
            for (size_t c = 0; c < chunks.size(); ++c) {
                auto const &chunk = chunks[c];
                Check(chunk.tile < tiles.size() &&
                      c >= tiles[chunk.tile].firstChunk &&
                      c < tiles[chunk.tile].firstChunk + tiles[chunk.tile].chunkCount,
                      "chunk tile ID agrees with tile interval");
            }
        }
    }
    desc.tileTarget = 64;
    UsdGenGraph canonical;
    result = compiler.Compile(desc, &canonical);
    Check(result.ok && canonical.Repartition(100000, 4),
          "canonical 100k repartition reports layout");
    Check(canonical.Tiles().size() == 49,
          "100k/512/tileTarget64 remains 49 tiles");
}

}  // namespace

int main()
{
    usdGenRegisterM1Operators();
    CheckPartitionBoundaries();

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
        if (pub.hairId.empty()) idArrays = false;
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
    Check(idArrays, "hairId uniform float array present on every tile (C2)");
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
        // T0 scope: BuildTileDataSource is the pure data-source assembler,
        // so the observable surface here is the container tree the scene
        // index would hand Hydra. The prim-type token (basisCurves) is a
        // property of HdSceneIndexPrim, asserted in the T1 imaging test.
        HdContainerDataSourceHandle c = UsdGenTilePublisher::BuildTileDataSource(pub);
        Check(bool(c), "BuildTileDataSource returns a container");
        if (c) {
            std::set<std::string> names = ChildNames(c);
            // Contract container is `basisCurves` (06 §4.1); the publisher
            // also carries displayStyle/purpose/visibility/materialBindings/
            // primOrigin/__dependencies/generation alongside it.
            for (char const *req : { "basisCurves", "primvars", "extent", "xform",
                                     "displayStyle", "purpose", "visibility",
                                     "materialBindings", "primOrigin" }) {
                Check(names.count(req) != 0,
                      std::string("C2 child '") + req + "' published");
            }
            if (HdContainerDataSourceHandle topo = HdContainerDataSource::Cast(
                    c->Get(TfToken("basisCurves")))) {
                // C2 (docs/freezes/C2.md:20-23): Hydra nests topology ONE
                // level down — basisCurves/topology/curveVertexCounts, with
                // type/basis/wrap as SIBLINGS of the topology container
                // (hd/basisCurvesSchema.h:38). A flat layout serves Storm no
                // topology and the prim is silently dropped (2026-09-12:
                // 49 published tiles, itemsDrawn == 1).
                std::set<std::string> tn = ChildNames(topo);
                for (char const *req : { "topology", "type", "basis", "wrap" }) {
                    Check(tn.count(req) != 0,
                          std::string("basisCurves child '") + req + "' published");
                }
                Check(LeafIsToken(LeafValue(topo->Get(TfToken("type"))), "cubic"),
                      "basisCurves type == cubic (contract constant, C2)");
                Check(LeafIsToken(LeafValue(topo->Get(TfToken("wrap"))), "pinned"),
                      "basisCurves wrap == pinned (contract constant, C2)");
                Check(LeafIsToken(LeafValue(topo->Get(TfToken("basis"))), "bspline"),
                      "basisCurves basis == usdGen:curve:basis (bspline)");
                if (HdContainerDataSourceHandle inner =
                        HdContainerDataSource::Cast(topo->Get(TfToken("topology")))) {
                    Check(ChildNames(inner).count("curveVertexCounts") != 0,
                          "basisCurves/topology/curveVertexCounts published");
                } else {
                    Check(false,
                          "basisCurves/topology container published (Hd nesting, C2)");
                }
            } else {
                Check(false, "basisCurves container published");
            }
            if (HdContainerDataSourceHandle primvars = HdContainerDataSource::Cast(
                    c->Get(TfToken("primvars")))) {
                std::set<std::string> pv = ChildNames(primvars);
                Check(pv.count("usdGenId") == 0,
                      "usdGenId is NOT published in M1 (S30 reserved)");
                Check(pv.count("normals") == 0,
                      "normals NEVER published (forces Storm ribbon key, C2)");
                // Always emitted: points/widths/hairT/hairId plus blocked
                // velocities/accelerations sentinels. st/displayColor/
                // bakeColor/extra planes ride only when the engine emits
                // them (look bake / bound data).
                for (char const *req : { "points", "widths", "hairT", "hairId",
                                         "velocities", "accelerations" }) {
                    Check(pv.count(req) != 0,
                          std::string("primvar '") + req + "' published");
                }
            }
            if (HdContainerDataSourceHandle style = HdContainerDataSource::Cast(
                    c->Get(TfToken("displayStyle")))) {
                Check(LeafIsInt(LeafValue(style->Get(TfToken("refineLevel"))), 2),
                      "displayStyle/refineLevel == 2 (C2, no M1 tumble tier)");
            }
            // Purpose: omitted when the publication carries none (unauthored
            // description purpose) so Hydra resolves the geometry render tag;
            // publishing purpose="default" would match NO tag and Storm would
            // never sync the tile (2026-09-12: 49 tiles, itemsDrawn == 1).
            // Authored values pass through verbatim (they ARE render tags).
            if (pub.purpose.IsEmpty()) {
                Check(ChildNames(c).count("purpose") == 0,
                      "no purpose container when publication purpose is empty "
                      "(-> geometry render tag)");
            }
            {
                UsdGenTilePublication authored = pub;
                authored.purpose = TfToken("render");
                HdContainerDataSourceHandle ca =
                    UsdGenTilePublisher::BuildTileDataSource(authored);
                HdContainerDataSourceHandle pa = HdContainerDataSource::Cast(
                    ca ? ca->Get(TfToken("purpose")) : nullptr);
                Check(bool(pa) &&
                          LeafIsToken(LeafValue(pa->Get(TfToken("purpose"))),
                                      "render"),
                      "authored purpose passes through verbatim");
                UsdGenTilePublication bare = pub;
                bare.purpose = TfToken();
                HdContainerDataSourceHandle cb =
                    UsdGenTilePublisher::BuildTileDataSource(bare);
                Check(cb && ChildNames(cb).count("purpose") == 0,
                      "no purpose container when publication purpose is empty "
                      "(-> geometry render tag)");
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
                HdContainerDataSourceHandle da =
                    UsdGenTilePublisher::BuildTileDataSource(a);
                HdContainerDataSourceHandle db =
                    UsdGenTilePublisher::BuildTileDataSource(b);
                if (da && db && !ValuesIdentical(HdDataSourceBaseHandle(da),
                                                 HdDataSourceBaseHandle(db)))
                    dsIdentical = false;
            }
        }
        Check(identical, "re-commit of identical desc publishes identical tile payloads");
        Check(dsIdentical, "re-commit data sources compare value-identical");
    }

    if (gen2 && !gen2->tiles.empty() && !gen->tiles.empty()) {
        // Additive overload (06 §4.1): the container can carry a prim-level
        // `generation` int source with the snapshot id.
        HdContainerDataSourceHandle ca =
            UsdGenTilePublisher::BuildTileDataSource(gen->tiles[0]);
        HdContainerDataSourceHandle cb =
            UsdGenTilePublisher::BuildTileDataSource(gen2->tiles[0], int64_t(gen2->id));
        if (ca && cb) {
            VtValue sv = LeafValue(cb->Get(TfToken("generation")));
            bool stampOk = (sv.IsHolding<int>() &&
                            int64_t(sv.UncheckedGet<int>()) == int64_t(gen2->id)) ||
                           (sv.IsHolding<int64_t>() &&
                            sv.UncheckedGet<int64_t>() == int64_t(gen2->id));
            Check(stampOk, "stamped generation source carries the snapshot id");
        }
    }

    // ---- Path shapes (SI-1): nested, never flat-with-slashes -------------
    {
        Check(UsdGenTilePublisher::RenderNamespace() == TfToken("__usdGenRender"),
              "render namespace token is __usdGenRender (single source of truth)");
        SdfPath tp = UsdGenTilePublisher::TilePath(descPath, UsdGenTileId(7));
        Check(tp == descPath.AppendChild(TfToken("__usdGenRender"))
                        .AppendChild(TfToken("tile_0007")),
              std::string("TilePath nested, zero-padded: ") + tp.GetText());
        Check(tp.GetString().find("tile_0007") != std::string::npos &&
                  tp.GetPathElementCount() == 3,
              "tile path is 3 elements deep (desc/__usdGenRender/tile_%04u)");
        SdfPath gp = UsdGenTilePublisher::GuidePath(descPath, TfToken("guideA"));
        Check(gp == descPath.AppendChild(TfToken("__usdGenRender"))
                        .AppendChild(TfToken("guides"))
                        .AppendChild(TfToken("guideA")),
              std::string("GuidePath nested: ") + gp.GetText());
    }

    // ---- Dirty -> notice mapping (06 §5.1 channel discipline) -----------
    // NoticesFor is a pure static of the publisher, so the mapping is graded
    // unconditionally: the §5.1 bare-leaf precondition (no caching filter
    // between usdGen and the render index) only constrains a live engine
    // chain, never these unit assertions. A host-enabled caching scene
    // index is reported, not graded around.
    if (char const *cach =
            std::getenv("USDIMAGINGGL_ENGINE_ENABLE_CACHING_SCENE_INDEX");
        cach && cach[0] != '\0' && std::string(cach) != "0" &&
            std::string(cach) != "false" && std::string(cach) != "off") {
        std::printf("[info] caching scene index enabled (%s); mapping still "
                    "graded (pure publisher statics)\n", cach);
    }
    {
        auto has = [](std::vector<HdDataSourceLocator> const &v,
                      std::vector<TfToken> const &path) {
            for (HdDataSourceLocator const &loc : v) {
                if (loc.GetElementCount() != path.size())
                    continue;
                bool eq = true;
                for (size_t i = 0; i < path.size(); ++i)
                    eq = eq && loc.GetElement(i) == path[i];
                if (eq)
                    return true;
            }
            return false;
        };
        const TfToken pv("primvars"), val("primvarValue"), pts("points"),
            wdt("widths"), ext("extent"), mn("min"), mx("max");

        // §5.1 row 1: points -> bare leaf + extent/min + extent/max.
        UsdGenTileDirty pointsOnly;
        pointsOnly.primPath = descPath;
        pointsOnly.pointsDirty = true;
        auto nPts = UsdGenTilePublisher::NoticesFor(pointsOnly).all();
        Check(has(nPts, { pv, pts, val }) && has(nPts, { ext, mn }) &&
                  has(nPts, { ext, mx }),
              "points dirty -> bare primvars/points/primvarValue + extent min/max");
        Check(!has(nPts, { pv, wdt, val }),
              "points-only notice carries no widths locator");

        // §5.1 row 2: a width-writing node is routed by the engine as an
        // ADDITIONAL dirtyPrimvars entry; the publisher must surface the
        // widths leaf alongside points, never as a container sentinel.
        UsdGenTileDirty pointsPlusWidths;
        pointsPlusWidths.primPath = descPath;
        pointsPlusWidths.pointsDirty = true;
        pointsPlusWidths.dirtyPrimvars = { wdt };
        auto nW = UsdGenTilePublisher::NoticesFor(pointsPlusWidths).all();
        Check(has(nW, { pv, pts, val }) && has(nW, { pv, wdt, val }),
              "width-writing-node dirty -> points AND widths leaves");

        // A widths-only delta must NOT invalidate points (whole-prim scatter).
        UsdGenTileDirty widthsOnly;
        widthsOnly.primPath = descPath;
        widthsOnly.widthsDirty = true;
        widthsOnly.dirtyPrimvars = { TfToken("st") };
        auto nw = UsdGenTilePublisher::NoticesFor(widthsOnly).all();
        Check(!has(nw, { pv, pts, val }) && !has(nw, { ext, mn }) &&
                  !has(nw, { ext, mx }),
              "widths-only notice carries no points or extent locator");
        Check(has(nw, { pv, TfToken("st"), val }),
              "dirtyPrimvars surfaces a primvars/st/primvarValue dirty locator");

        // §5.1 row 4: a primvar appearing for the first time is announced as
        // primvars/<name> ONCE — never a /primvarValue dirty (that would not
        // make Storm build the container). An added tile carries its whole
        // new-primvar set in newPrimvars.
        UsdGenTileDirty added;
        added.primPath = descPath;
        added.added = true;
        added.newPrimvars = { TfToken("st"), TfToken("displayColor") };
        auto nAdd = UsdGenTilePublisher::NoticesFor(added);
        bool allBare = !nAdd.newPrimvarLocators.empty();
        for (HdDataSourceLocator const &loc : nAdd.newPrimvarLocators) {
            allBare = allBare && loc.GetElementCount() == 2 &&
                loc.GetElement(0) == pv;
        }
        Check(allBare,
              "added tile announces each new primvar once, bare, no leaf dirty");
        auto nAddAll = nAdd.all();
        Check(has(nAddAll, { pv, TfToken("st") }) &&
                  has(nAddAll, { pv, TfToken("displayColor") }),
              "added-tile notice contains primvars/st and primvars/displayColor");

        // xform-only delta: the tile's own xform/matrix and nothing else
        // (§5.1: Hydra dirtiness is per prim, never hierarchical).
        UsdGenTileDirty xformOnly;
        xformOnly.primPath = descPath;
        xformOnly.xformDirty = true;
        auto nx = UsdGenTilePublisher::NoticesFor(xformOnly).all();
        Check(has(nx, { TfToken("xform"), TfToken("matrix") }) &&
                  !has(nx, { pv, pts, val }) && !has(nx, { pv, wdt, val }),
              "xform-only delta is xform-only (no republish, S4)");
    }

    return g_failures ? 1 : 0;
}
