#ifndef USDGEN_IMAGING_DEVICE_TILE_PUBLISHER_H
#define USDGEN_IMAGING_DEVICE_TILE_PUBLISHER_H


#include "pxr/pxr.h"
#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

struct UsdGenDeviceTileMetadata {
    SdfPath primPath;
    uint32_t tile = 0;
    TfToken curveType;
    TfToken curveBasis;
    TfToken curveWrap;
    int refineLevel = 2;
    GfVec3d extentMin = GfVec3d(0.0);
    GfVec3d extentMax = GfVec3d(0.0);
    GfMatrix4d xform = GfMatrix4d(1.0);
    TfToken purpose;
    TfToken visibility;
    SdfPath materialPath;
    TfToken materialPurpose = TfToken("allPurpose");
    SdfPath primOrigin;
    SdfPath dependencySurface;
    int64_t generation = -1;
};

/// Builds a device-only BasisCurves prim. The provider owns the GPU channels
/// and is intentionally opaque here; no host points/counts are accepted.
HdContainerDataSourceHandle BuildDeviceTileDataSource(
    UsdGenDeviceTileMetadata const &metadata,
    HdDataSourceBaseHandle const &provider);

} // namespace usdGenImaging

#endif
