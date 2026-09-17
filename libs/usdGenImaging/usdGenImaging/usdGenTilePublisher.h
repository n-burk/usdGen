// usdGen imaging — tile publisher: engine generation -> Hydra prims +
// notices (contract C2, docs/freezes/C2.md; 06-imaging.md §4/§5.1).
//
// Publishes every usdGen::UsdGenTilePublication as
// <description>/__usdGenRender/tile_NNNN (basisCurves, C2 payload) and turns
// a usdGen::UsdGenDirtyReport into the exact notice set of 06 §5.1:
//   * points change      -> primvars/points/primvarValue + extent/min +
//                           extent/max  (bare leaf; never the container
//                           sentinel from ComputeDirtyLocators)
//   * widths change      -> primvars/widths/primvarValue
//   * xform change       -> xform/matrix
//   * primvar appears    -> primvars/<name> ONCE (never /primvarValue)
//   * colour-only edit   -> primvars/displayColor/primvarValue ALONE
//                           (never co-dirtied with points — DirtyPrimvar
//                           re-uploads every non-points primvar, 06 §5.1)
//   * prim appears       -> PrimsAdded; removed -> PrimsRemoved
// Notice order: PrimsRemoved, PrimsAdded, PrimsDirtied (06 §3.9).
// No caching filter may sit between usdGen and the render index (06 §5.1
// precondition — asserted by testUsdGenTileContract).
//
// The engine fills EVERY UsdGenTilePublication field at commit time
// (curveBuffer.h: "the engine only fills them"): points/widths/hairT/hairId/
// st/displayColor/extent/xformMatrix/primOrigin/materialPath/primPath. This
// class is a pure data-source assembler over the published struct — it
// reads it, never writes to it.
#ifndef USDGEN_IMAGING_TILE_PUBLISHER_H
#define USDGEN_IMAGING_TILE_PUBLISHER_H

#include "usdGenImaging/api.h"
#include "usdGen/curveBuffer.h"

#include "pxr/pxr.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/usd/sdf/path.h"

#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

class UsdGenTilePublisher
{
public:
    /// The C2 data source for one tile prim (basisCurves/topology/*,
    /// primvars/*, extent/*, xform/*, displayStyle/refineLevel, purpose,
    /// visibility, materialBindings, primOrigin, __dependencies).
    /// Block datasources for velocities/accelerations outside motion P1.
    /// Hard-asserts points.size() == sum(curveVertexCounts) per tile (SI-1).
    static HdContainerDataSourceHandle BuildTileDataSource(
        usdGen::UsdGenTilePublication const &tile);
    /// Generation-stamped variant (StormSurgery P0 contract): stamps a
    /// prim-level `generation` int with the publication snapshot id. The
    /// legacy overload above stays unstamped (SI-1 callers).
    static HdContainerDataSourceHandle BuildTileDataSource(
        usdGen::UsdGenTilePublication const &tile, int64_t generation);

    /// The C2 data source for a guide prim (06 §4.2): basisCurves,
    /// purpose = guide, primvars/usdGen:role = "guide".
    static HdContainerDataSourceHandle BuildGuideDataSource(
        usdGen::UsdGenTilePublication const &guide,
        std::string const &setName);

    /// Exact notice locator sets for one tile (06 §5.1 rules).
    struct TileNotices
    {
        bool pointsDirty = false;
        bool widthsDirty = false;
        bool xformDirty = false;
        std::vector<HdDataSourceLocator> newPrimvarLocators;    // primvars/<name>
        std::vector<HdDataSourceLocator> dirtyPrimvarLocators;  // primvars/<name>/primvarValue
        std::vector<HdDataSourceLocator> all() const;          // union, order: points, widths, xform, new, dirty
    };

    static TileNotices NoticesFor(usdGen::UsdGenTileDirty const &tileDirty);

    /// Tile prim path: <description>/__usdGenRender/tile_%04d (C2 naming).
    static SdfPath TilePath(SdfPath const &descriptionPath, usdGen::UsdGenTileId tile);

    /// Guides: <description>/__usdGenRender/guides/<setName> (+ /cvs child).
    static SdfPath GuidePath(SdfPath const &descriptionPath, TfToken const &setName);

    /// The synthetic default-material prim (06 §4.4 `material_storm` slot):
    /// <description>/__usdGenRender/material_storm. Tiles bind it whenever
    /// the description authors no material of its own, so Storm shades hair
    /// with the UsdGenHairPreview glslfx instead of falling back to flat
    /// displayColor. UsdGenGroomSceneIndex synthesizes the prim.
    static SdfPath MaterialPath(SdfPath const &descriptionPath);

    /// The same prim derived from a published tile path
    /// (<description>/__usdGenRender/tile_NNNN) — the publisher only ever
    /// sees the tile path.
    static SdfPath DefaultMaterialPath(SdfPath const &tilePath);

    /// The `material` prim data source for MaterialPath(): one node named
    /// `surface` whose nodeIdentifier is the Sdr id `UsdGenHairPreview`
    /// (bind by identifier, never an asset path — see
    /// usdGenShaders/resources/hairLook.usda), wired to the universal
    /// render context's `surface` terminal. Parameters are left unset so the
    /// C5-frozen Sdr defaults apply.
    static HdContainerDataSourceHandle BuildDefaultMaterialDataSource();

    /// The synthetic value-preview materials (usdGen:preview:*):
    /// <description>/__usdGenRender/material_preview (lit) and
    /// material_preview_flat. The engine binds one of them
    /// (usdGen::UsdGenPreviewMaterialPath) on every tile of a previewing
    /// description; UsdGenGroomSceneIndex serves whichever the tiles bind.
    static SdfPath PreviewMaterialPath(SdfPath const &descriptionPath, bool flat);

    /// True when `path` is one of the two preview materials of
    /// `descriptionPath`; `flat` says which.
    static bool IsPreviewMaterialPath(SdfPath const &descriptionPath,
                                      SdfPath const &path, bool *flat = nullptr);

    /// The `material` prim data source of a preview material: the
    /// `UsdGenValuePreview` glslfx, which shows displayColor as it is
    /// (`shading` 0) or darkened where strands turn away (`shading` 1).
    static HdContainerDataSourceHandle BuildPreviewMaterialDataSource(bool flat);

    /// The reserved name of the synthetic default material prim.
    static TfToken const &MaterialName()
    {
        static TfToken const token("material_storm");
        return token;
    }

    /// The reserved Hydra-only render-namespace child of a description
    /// (06 §4, 02 §0.6).
    static TfToken const &RenderNamespace()
    {
        static TfToken const token("__usdGenRender");
        return token;
    }
};

}  // namespace usdGenImaging

#endif  // USDGEN_IMAGING_TILE_PUBLISHER_H
