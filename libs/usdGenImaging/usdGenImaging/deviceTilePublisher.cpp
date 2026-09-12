#include "usdGenImaging/deviceTilePublisher.h"

#include "pxr/imaging/hd/dependencySchema.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include <cmath>
#include "pxr/imaging/hd/xformSchema.h"

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdGenImaging {
namespace {
template<class T> HdSampledDataSourceHandle _S(T const &v) {
    return HdRetainedTypedSampledDataSource<T>::New(v);
}
HdDataSourceBaseHandle _Tok(TfToken const &v) { return _S(v); }
bool _PrimPath(SdfPath const &p) {
    return !p.IsEmpty() && p.IsAbsolutePath() && p.IsPrimPath() &&
        p != SdfPath::AbsoluteRootPath();
}
bool _CurveTokenValid(TfToken const &type, TfToken const &basis,
                      TfToken const &wrap) {
    const bool linear = type == TfToken("linear");
    const bool cubic = type == TfToken("cubic");
    const bool validBasis = (linear && basis == TfToken("linear")) ||
        ((linear || cubic) && (basis == TfToken("bezier") ||
                   basis == TfToken("bspline") ||
                   basis == TfToken("catmullRom") ||
                   basis == TfToken("centripetalCatmullRom")));
    const bool validWrap = wrap == TfToken("periodic") ||
        wrap == TfToken("nonperiodic") || wrap == TfToken("pinned") ||
        wrap == TfToken("segmented");
    return (linear || cubic) && validBasis && validWrap;
}
HdContainerDataSourceHandle _C(std::vector<TfToken> const &n,
                               std::vector<HdDataSourceBaseHandle> const &v) {
    return HdRetainedContainerDataSource::New(n.size(), n.data(), v.data());
}
}

HdContainerDataSourceHandle BuildDeviceTileDataSource(
    UsdGenDeviceTileMetadata const &m, HdDataSourceBaseHandle const &provider)
{
    if (!provider || !m.primPath.IsPrimPath() || !m.primPath.IsAbsolutePath() ||
        m.primPath.IsEmpty() ||
        m.generation < 0 || !_CurveTokenValid(m.curveType, m.curveBasis,
        m.curveWrap) || m.refineLevel < 0 ||
        m.extentMin[0] > m.extentMax[0] ||
        m.extentMin[1] > m.extentMax[1] || m.extentMin[2] > m.extentMax[2])
        return {};
    if ((!m.purpose.IsEmpty() && m.purpose == TfToken("default")) ||
        (!m.visibility.IsEmpty() && m.visibility != TfToken("inherited") &&
         m.visibility != TfToken("invisible")))
        return {};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(m.extentMin[i]) || !std::isfinite(m.extentMax[i]))
            return {};
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(m.xform[r][c])) return {};
    if ((!m.materialPath.IsEmpty() && (!_PrimPath(m.materialPath) ||
         m.materialPurpose.IsEmpty())) ||
        (!m.primOrigin.IsEmpty() && !_PrimPath(m.primOrigin)) ||
        (!m.dependencySurface.IsEmpty() && !_PrimPath(m.dependencySurface)))
        return {};
    auto topology = HdBasisCurvesTopologySchema::Builder()
        .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(m.curveType))
        .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(m.curveBasis))
        .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(m.curveWrap))
        .Build();
    auto basisCurves = HdBasisCurvesSchema::Builder().SetTopology(topology).Build();
    auto extent = _C(
        {TfToken("min"), TfToken("max")}, {_S(m.extentMin), _S(m.extentMax)});
    auto xform = HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(m.xform))
        .SetResetXformStack(HdRetainedTypedSampledDataSource<bool>::New(true))
        .Build();
    std::vector<TfToken> names{TfToken("basisCurves"), TfToken("extent"),
        TfToken("xform"), TfToken("hdStBasisCurvesGpu"),
        TfToken("generation"), TfToken("displayStyle")};
    std::vector<HdDataSourceBaseHandle> values{basisCurves, extent, xform,
        provider, _S(m.generation), _C({TfToken("refineLevel")},
            {_S(m.refineLevel)})};
    if (!m.purpose.IsEmpty()) {
        names.push_back(TfToken("purpose"));
        values.push_back(_C({TfToken("purpose")}, {_Tok(m.purpose)}));
    }
    if (!m.visibility.IsEmpty()) {
        names.push_back(TfToken("visibility"));
        values.push_back(HdVisibilitySchema::Builder().SetVisibility(
            HdRetainedTypedSampledDataSource<bool>::New(
                m.visibility != TfToken("invisible"))).Build());
    }
    if (!m.materialPath.IsEmpty()) {
        HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder()
            .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(
                m.materialPath)).Build();
        names.push_back(TfToken("materialBindings"));
        values.push_back(_C({m.materialPurpose}, {binding}));
    }
    if (!m.primOrigin.IsEmpty()) {
        names.push_back(TfToken("primOrigin"));
        values.push_back(_C({TfToken("scenePath")}, {_S(m.primOrigin)}));
    }
    if (!m.dependencySurface.IsEmpty()) {
        HdDataSourceLocator const points(
            TfToken("primvars"), TfToken("points"));
        auto dependency = HdDependencySchema::Builder()
            .SetDependedOnPrimPath(
                HdRetainedTypedSampledDataSource<SdfPath>::New(
                    m.dependencySurface))
            .SetDependedOnDataSourceLocator(
                HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(points))
            .SetAffectedDataSourceLocator(
                HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
                    HdDataSourceLocator(TfToken("hdStBasisCurvesGpu"))))
            .Build();
        names.push_back(TfToken("__dependencies"));
        values.push_back(_C({TfToken("usdGenSurface")}, {dependency}));
    }
    return _C(names, values);
}
} // namespace usdGenImaging
