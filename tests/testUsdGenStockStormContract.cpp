// Stock-Hydra data-source contract: this intentionally uses only public
// BasisCurves and primvar schema payloads.  It does not load or reference a
// private HdSt provider.
#include "usdGen/curveBuffer.h"
#include "usdGenImaging/usdGenTilePublisher.h"

#include "pxr/pxr.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/dataSource.h"

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool
Check(bool const condition, char const *const message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

HdContainerDataSourceHandle
ContainerChild(HdContainerDataSourceHandle const &container,
               TfToken const &name)
{
    return container ? HdContainerDataSource::Cast(container->Get(name))
                     : HdContainerDataSourceHandle();
}

template <class T>
bool
SampleEquals(HdContainerDataSourceHandle const &container,
             TfToken const &name, T const &expected)
{
    HdSampledDataSourceHandle const value = HdSampledDataSource::Cast(
        container ? container->Get(name) : HdDataSourceBaseHandle());
    return value && value->GetValue(0.0).IsHolding<T>() &&
        value->GetValue(0.0).UncheckedGet<T>() == expected;
}

} // namespace

int
main()
{
    usdGen::UsdGenTilePublication tile;
    tile.primPath = SdfPath("/stock/tile_0000");
    tile.curveVertexCounts = VtIntArray{2, 3};
    tile.points = VtVec3fArray{
        GfVec3f(0.0f, 0.0f, 0.0f), GfVec3f(0.0f, 1.0f, 0.0f),
        GfVec3f(1.0f, 0.0f, 0.0f), GfVec3f(1.0f, 1.0f, 0.0f),
        GfVec3f(1.0f, 2.0f, 0.0f)};
    tile.widths = VtFloatArray{0.1f, 0.2f, 0.3f, 0.4f, 0.5f};

    HdContainerDataSourceHandle const root =
        usdGenImaging::UsdGenTilePublisher::BuildTileDataSource(tile);
    HdContainerDataSourceHandle const basis =
        ContainerChild(root, TfToken("basisCurves"));
    HdContainerDataSourceHandle const topology =
        ContainerChild(basis, TfToken("topology"));
    HdContainerDataSourceHandle const primvars =
        ContainerChild(root, TfToken("primvars"));
    HdContainerDataSourceHandle const points =
        ContainerChild(primvars, TfToken("points"));
    HdContainerDataSourceHandle const widths =
        ContainerChild(primvars, TfToken("widths"));

    bool okay = true;
    okay &= Check(root && basis && topology && primvars,
                  "public basisCurves/topology/primvars containers exist");
    okay &= Check(!root->Get(TfToken("hdStBasisCurvesGpu")),
                  "stock payload does not expose a private GPU datasource");
    okay &= Check(SampleEquals(topology, TfToken("curveVertexCounts"),
                               tile.curveVertexCounts),
                  "standard curveVertexCounts preserve ragged topology");
    okay &= Check(SampleEquals(points, TfToken("primvarValue"), tile.points),
                  "standard points primvar preserves every CV");
    okay &= Check(SampleEquals(widths, TfToken("primvarValue"), tile.widths),
                  "standard widths primvar preserves every CV");
    okay &= Check(SampleEquals(points, TfToken("interpolation"),
                               TfToken("vertex")) &&
                  SampleEquals(widths, TfToken("interpolation"),
                               TfToken("vertex")),
                  "points and widths use standard vertex primvars");
    return okay ? 0 : 1;
}
