// usdGen imaging — C2 tile publication -> Hydra retained data sources.
//
// Assembly contract: plan/06-imaging.md §4.1 (the C2 table; docs/freezes/C2.md
// carries the frozen field list). The engine fills EVERY field of
// UsdGenTilePublication at commit time (curveBuffer.h) — this file reads it
// and never writes to it (no hashing, no hairId derivation here).
#include "usdGenImaging/usdGenTilePublisher.h"

#include "usdGenImaging/usdGenTokens.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/dependencySchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/xformSchema.h"

#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

namespace {

// All payload names are spelled as literal TfTokens: TfToken interning makes
// them identical to the corresponding HdTokens without pulling the whole
// token table into the diff of this contract file.
HdTokenDataSourceHandle
_Tok(TfToken const &t)
{
    return HdRetainedTypedSampledDataSource<TfToken>::New(t);
}

template <class T>
HdSampledDataSourceHandle
_Samp(T const &v)
{
    return HdRetainedTypedSampledDataSource<T>::New(v);
}

HdDataSourceBaseHandle
_Block()
{
    return HdDataSourceBaseHandle(HdBlockDataSource::New());
}

// One primvars/<name> entry: { values, interpolation, type?, role?,
// elementSize? } (06 §4.1; interpolation NEVER "varying").
HdContainerDataSourceHandle
_Primvar(HdSampledDataSourceHandle const &values,
         TfToken const &interpolation,
         TfToken const &role = TfToken(),
         int elementSize = 0)
{
    HdPrimvarSchema::Builder b;
    b.SetPrimvarValue(values);
    if (!interpolation.IsEmpty()) {
        b.SetInterpolation(_Tok(interpolation));
    }
    if (!role.IsEmpty()) {
        b.SetRole(_Tok(role));
    }
    if (elementSize > 0) {
        b.SetElementSize(HdRetainedTypedSampledDataSource<int>::New(elementSize));
    }
    return b.Build();
}

void
_Add(std::vector<TfToken> *names, std::vector<HdDataSourceBaseHandle> *values,
     TfToken const &name, HdDataSourceBaseHandle const &ds)
{
    if (ds) {
        names->push_back(name);
        values->push_back(ds);
    }
}

HdContainerDataSourceHandle
_Container(std::vector<TfToken> &&names,
           std::vector<HdDataSourceBaseHandle> &&values)
{
    return HdRetainedContainerDataSource::New(
        names.size(), names.data(), values.data());
}

HdContainerDataSourceHandle
_Topology(usdGen::UsdGenTilePublication const &tile)
{
    // C2 (docs/freezes/C2.md:20-23; 06 §4.1): the Hydra BasisCurves schema
    // nests topology ONE level down — basisCurves/topology/curveVertexCounts
    // with type/basis/wrap as SIBLINGS of the topology container
    // (pxr/imaging/hd/basisCurvesSchema.h:38 — the schema token IS
    // "topology"). A flat layout serves no topology to Storm, which drops
    // the prim silently (2026-09-12: 49 published tiles, itemsDrawn == 1).
    // NOTE: basis carries the open/closed semantics; open is Hydra-exact.
    return HdRetainedContainerDataSource::New(
        /*name1*/ TfToken("topology"),
        /*value1*/ HdRetainedContainerDataSource::New(
            /*tname1*/ TfToken("curveVertexCounts"),
            /*tvalue1*/ _Samp(tile.curveVertexCounts)),
        /*name2*/ TfToken("type"),
        /*value2*/ _Tok(TfToken("cubic")),
        /*name3*/ TfToken("basis"),
        /*value3*/ _Tok(tile.basis.empty() ? TfToken("bezier")
                                           : TfToken(tile.basis)),
        /*name4*/ TfToken("wrap"),
        /*value4*/ _Tok(TfToken("pinned")));
}

// size-derived interpolation for per-curve / per-CV colour arrays (06 §4.1
// displayColor row: uniform per curve, vertex per CV).
TfToken
_CurveOrVertexInterp(size_t n, size_t numCurves, size_t totalCvs,
                     char const *what, SdfPath const &path)
{
    if (n == numCurves) {
        return TfToken("uniform");
    }
    if (n == totalCvs) {
        return TfToken("vertex");
    }
    TF_CODING_ERROR("usdGen tile %s: primvars/%s has %zu entries, which is "
                    "neither one per curve (%zu) nor one per CV (%zu).",
                    path.GetText(), what, n, numCurves, totalCvs);
    return TfToken("uniform");
}

HdContainerDataSourceHandle
_Assemble(usdGen::UsdGenTilePublication const &tile, bool isGuide,
          TfToken const &guideRole, int64_t generation = -1)
{
    size_t totalCvs = 0;
    for (int c : tile.curveVertexCounts) {
        totalCvs += size_t(c);
    }
    size_t const numCurves = tile.curveVertexCounts.size();
    if (numCurves == 0) {
        // S27 / 06 §4: an empty basisCurves prim is NEVER published — the
        // engine drops empty tiles before Commit publishes them.
        TF_CODING_ERROR("usdGen tile %s: empty tile reached the publisher; "
                        "empty tiles must not be published (S27).",
                        tile.primPath.GetText());
        return nullptr;
    }
    if (tile.points.size() != totalCvs) {
        TF_CODING_ERROR("usdGen tile %s: points.size() (%zu) != sum of "
                        "curveVertexCounts (%zu).",
                        tile.primPath.GetText(),
                        size_t(tile.points.size()), totalCvs);
    }

    // --- primvars ---------------------------------------------------------
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;

    _Add(&pvNames, &pvValues, TfToken("points"),
         _Primvar(_Samp(tile.points), TfToken("vertex"), TfToken("point")));

    if (!tile.widths.empty()) {
        // widths: vertex, or CONSTANT for the single-tuple width attribute
        // — never "varying" (06 §4.1 widths row).
        _Add(&pvNames, &pvValues, TfToken("widths"),
             _Primvar(_Samp(tile.widths),
                      tile.widths.size() == 1 ? TfToken("constant")
                                              : TfToken("vertex")));
    }

    if (!tile.hairT.empty()) {
        _Add(&pvNames, &pvValues, TfToken("hairT"),
             _Primvar(_Samp(tile.hairT), TfToken("vertex")));
    }
    if (!tile.hairId.empty()) {
        _Add(&pvNames, &pvValues, TfToken("hairId"),
             _Primvar(_Samp(tile.hairId), TfToken("uniform")));
    }
    if (!tile.st.empty()) {
        _Add(&pvNames, &pvValues, TfToken("st"),
             _Primvar(_Samp(tile.st), TfToken("uniform"),
                      TfToken("textureCoordinate")));
    }
    if (!tile.displayColor.empty()) {
        _Add(&pvNames, &pvValues, TfToken("displayColor"),
             _Primvar(_Samp(tile.displayColor),
                      _CurveOrVertexInterp(tile.displayColor.size(),
                                           numCurves, totalCvs,
                                           "displayColor", tile.primPath)));
    }
    if (!tile.bakeColor.empty()) {
        // Contract gap: UsdGenTilePublication carries the bake colours but
        // NOT the authored bakePrimvar NAME; published under "bakeColor".
        _Add(&pvNames, &pvValues, TfToken("bakeColor"),
             _Primvar(_Samp(tile.bakeColor),
                      _CurveOrVertexInterp(tile.bakeColor.size(),
                                           numCurves, totalCvs,
                                           "bakeColor", tile.primPath)));
    }

    // Engine-internal uniform planes (clumpId_<level>, guideIndex/guideWeight
    // with elementSize = arity 3, minScreenSpaceWidths constant 1.0).
    for (usdGen::UsdGenPlane const &plane : tile.extraUniform) {
        HdSampledDataSourceHandle values;
        if (plane.type == "int") {
            values = _Samp(plane.i);
        } else {
            values = _Samp(plane.f);
        }
        _Add(&pvNames, &pvValues, plane.name,
             _Primvar(values, plane.interpolation, TfToken(), plane.arity));
    }

    // Motion (06 §4.1 velocities row): real data in P1, an HdBlockDataSource
    // on P0/P2 so an authored primvar on the C3 source cannot leak through.
    if (!tile.velocities.empty()) {
        _Add(&pvNames, &pvValues, TfToken("velocities"),
             _Primvar(_Samp(tile.velocities), TfToken("vertex")));
        _Add(&pvNames, &pvValues, TfToken("accelerations"), _Block());
    } else {
        _Add(&pvNames, &pvValues, TfToken("velocities"), _Block());
        _Add(&pvNames, &pvValues, TfToken("accelerations"), _Block());
    }

    if (isGuide) {
        // 06 §4.2: guides carry primvars/usdGen:role = guide.
        _Add(&pvNames, &pvValues, TfToken("usdGen:role"),
             _Primvar(_Tok(guideRole), TfToken("constant")));
    }
    // NO primvars/normals, NO primvars/hairTangent (06 §4.1: tangent is a
    // Storm extension opt-in, never default-on).

    // --- the rest of the prim container ------------------------------------
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;

    _Add(&names, &values, TfToken("basisCurves"), _Topology(tile));
    // StormSurgery generation stamp (P0 race contract): the publication
    // snapshot id, threaded as a parameter (the engine tile carries no
    // generation field — libs/usdGen, not this lane). -1 = unstamped.
    if (generation >= 0) {
        _Add(&names, &values, TfToken("generation"), _Samp((int)generation));
    }
    _Add(&names, &values, TfToken("primvars"),
         _Container(std::move(pvNames), std::move(pvValues)));

    _Add(&names, &values, TfToken("extent"),
         _Container(
             {TfToken("min"), TfToken("max")},
             {HdDataSourceBaseHandle(_Samp(tile.extentMin)),
              HdDataSourceBaseHandle(_Samp(tile.extentMax))}));

    // UsdImaging xform semantics: our own matrix must REPLACE the inherited
    // surface chain (02 §1.7), hence resetXformStack.
    _Add(&names, &values, TfToken("xform"),
         HdXformSchema::Builder()
             .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(
                 tile.xformMatrix))
             .SetResetXformStack(
                 HdRetainedTypedSampledDataSource<bool>::New(true))
             .Build());

    // Storm P0 (06 §4.1): the publication's refineLevel (engine default 2);
    // tessellation stays at the Osd default (S14: never USDGEN_TESSELLATION_LEVEL).
    // Storm P0 (06 §4.1): the publication's refineLevel (engine default 2);
    // tessellation stays at the Osd default (S14: never USDGEN_TESSELLATION_LEVEL).
    _Add(&names, &values, TfToken("displayStyle"),
         _Container({TfToken("refineLevel")},
                    {HdDataSourceBaseHandle(_Samp(tile.refineLevel))}));

    // Purpose: publish ONLY when authored. An empty purpose omits the
    // container, which Hydra resolves to the geometry render tag
    // (HdSceneIndexAdapterSceneDelegate::GetRenderTag falls back to geometry
    // iff the container is absent/empty). Publishing purpose="default" is
    // FATAL — "default" is not a render tag, so the tile matches no
    // collection and Storm never syncs it (2026-09-12: 49 published tiles,
    // itemsDrawn == 1, zero HdStBasisCurves sync lines). Authored values
    // (render/guide/proxy) pass through verbatim — they ARE render tags.
    if (!tile.purpose.IsEmpty()) {
        _Add(&names, &values, TfToken("purpose"),
             _Container({TfToken("purpose")},
                        {HdDataSourceBaseHandle(_Tok(tile.purpose))}));
    }
    _Add(&names, &values, TfToken("visibility"),
         _Container({TfToken("visibility")},
                    {HdDataSourceBaseHandle(_Tok(tile.visibility))}));

    if (!tile.materialPath.IsEmpty()) {
        // materialBindings/allPurpose/<empty-token binding> (06 §4.1).
        TfToken const bindingName;  // the empty-token child (R: binding slot)
        HdDataSourceBaseHandle binding(_Samp(tile.materialPath));
        _Add(&names, &values, TfToken("materialBindings"),
             _Container(
                 {tile.materialPurpose.IsEmpty()
                      ? TfToken("allPurpose")
                      : tile.materialPurpose},
                 {HdDataSourceBaseHandle(
                     HdRetainedContainerDataSource::New(
                         1, &bindingName, &binding))}));
    }
    if (!tile.primOrigin.IsEmpty()) {
        _Add(&names, &values, TfToken("primOrigin"),
             _Container({TfToken("scenePath")},
                        {HdDataSourceBaseHandle(_Samp(tile.primOrigin))}));
    }
    if (!tile.dependencySurface.IsEmpty()) {
        // Exactly ONE __dependencies entry: the bound surface's points feed
        // our points (02 §1.7; HdDependencySchema field names verified).
        HdDataSourceLocator const pointsLoc(
            TfToken("primvars"), TfToken("points"));
        HdContainerDataSourceHandle dep =
            HdDependencySchema::Builder()
                .SetDependedOnPrimPath(
                    HdRetainedTypedSampledDataSource<SdfPath>::New(
                        tile.dependencySurface))
                .SetDependedOnDataSourceLocator(
                    HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
                        pointsLoc))
                .SetAffectedDataSourceLocator(
                    HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
                        pointsLoc))
                .Build();
        _Add(&names, &values, TfToken("__dependencies"),
             _Container({TfToken("usdGenSurface")},
                        {HdDataSourceBaseHandle(dep)}));
    }

    return _Container(std::move(names), std::move(values));
}

}  // namespace

/*static*/
HdContainerDataSourceHandle
UsdGenTilePublisher::BuildTileDataSource(
    usdGen::UsdGenTilePublication const &tile)
{
    return _Assemble(tile, /*isGuide=*/false, TfToken());
}

/*static*/
HdContainerDataSourceHandle
UsdGenTilePublisher::BuildTileDataSource(
    usdGen::UsdGenTilePublication const &tile, int64_t generation)
{
    return _Assemble(tile, /*isGuide=*/false, TfToken(), generation);
}

/*static*/
HdContainerDataSourceHandle
UsdGenTilePublisher::BuildGuideDataSource(
    usdGen::UsdGenTilePublication const &guide,
    std::string const &setName)
{
    (void)setName;  // the set name lives in the prim path (GuidePath); the
                    // payload marker is primvars/usdGen:role = guide.
    return _Assemble(guide, /*isGuide=*/true, TfToken("guide"));
}

/*static*/
UsdGenTilePublisher::TileNotices
UsdGenTilePublisher::NoticesFor(usdGen::UsdGenTileDirty const &tileDirty)
{
    TileNotices n;
    n.pointsDirty = tileDirty.pointsDirty;
    n.widthsDirty = tileDirty.widthsDirty;
    n.xformDirty = tileDirty.xformDirty;
    n.newPrimvarLocators.reserve(tileDirty.newPrimvars.size());
    for (TfToken const &name : tileDirty.newPrimvars) {
        // A NEW primvar dirties primvars/<name> ONCE — never /primvarValue
        // (06 §5.1: the container-level dirty makes the client build it).
        n.newPrimvarLocators.emplace_back(TfToken("primvars"), name);
    }
    n.dirtyPrimvarLocators.reserve(tileDirty.dirtyPrimvars.size());
    for (TfToken const &name : tileDirty.dirtyPrimvars) {
        n.dirtyPrimvarLocators.emplace_back(
            TfToken("primvars"), name, TfToken("primvarValue"));
    }
    return n;
}

/*static*/
std::vector<HdDataSourceLocator>
UsdGenTilePublisher::TileNotices::all() const
{
    std::vector<HdDataSourceLocator> out;
    if (pointsDirty) {
        // Bare leaf + the two extent leaves; the container sentinel of
        // ComputeExtents is never used (06 §5.1).
        out.emplace_back(TfToken("primvars"), TfToken("points"),
                         TfToken("primvarValue"));
        out.emplace_back(TfToken("extent"), TfToken("min"));
        out.emplace_back(TfToken("extent"), TfToken("max"));
    }
    if (widthsDirty) {
        out.emplace_back(TfToken("primvars"), TfToken("widths"),
                         TfToken("primvarValue"));
    }
    if (xformDirty) {
        out.emplace_back(TfToken("xform"), TfToken("matrix"));
    }
    out.insert(out.end(), newPrimvarLocators.begin(), newPrimvarLocators.end());
    out.insert(out.end(), dirtyPrimvarLocators.begin(),
               dirtyPrimvarLocators.end());
    return out;
}

/*static*/
SdfPath
UsdGenTilePublisher::TilePath(
    SdfPath const &descriptionPath, usdGen::UsdGenTileId tile)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "tile_%04u", unsigned(tile));
    return descriptionPath.AppendChild(RenderNamespace())
        .AppendChild(TfToken(buf));
}

/*static*/
SdfPath
UsdGenTilePublisher::GuidePath(
    SdfPath const &descriptionPath, TfToken const &setName)
{
    return descriptionPath.AppendChild(RenderNamespace())
        .AppendChild(TfToken("guides"))
        .AppendChild(setName);
}

}  // namespace usdGenImaging

