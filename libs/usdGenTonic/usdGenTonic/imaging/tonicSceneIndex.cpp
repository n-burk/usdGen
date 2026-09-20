// usdGenTonic imaging — Tonic scene index implementation (plan/18 §2.1-§2.2).
#include "usdGenTonic/imaging/tonicSceneIndex.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicPublish.h"
#include "usdGenTonic/tonicRegistry.h"
#include "usdGenTonic/tonicTransport.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/materialConnectionSchema.h"
#include "pxr/imaging/hd/materialNetworkSchema.h"
#include "pxr/imaging/hd/materialNodeSchema.h"
#include "pxr/imaging/hd/materialNodeParameterSchema.h"
#include "pxr/imaging/hd/materialSchema.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/meshTopologySchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/usd/usdGeom/tokens.h"

#include <map>
#include <set>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(USDGENTONIC_ENABLE, true,
                      "Enable the usdGenTonic scene index plugin.");
TF_DEFINE_ENV_SETTING(USDGENTONIC_TEST_TUBE, true,
                      "Publish the static test tube while no model is "
                      "active. Headless records set 0 so the scaffolding "
                      "never lands in rendered frames. Activating a model "
                      "removes it either way.");
TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<UsdGenTonicSceneIndexPlugin>();
}
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("UsdGenTonicSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexBaseRefPtr
UsdGenTonicSceneIndexPlugin::_AppendSceneIndex(
    std::string const&, HdSceneIndexBaseRefPtr const& input,
    HdContainerDataSourceHandle const&) {
    return UsdGenTonicSceneIndex::New(input);
}

bool
UsdGenTonicSceneIndexPlugin::_IsEnabled(
    HdContainerDataSourceHandle const&) const {
    return TfGetEnvSetting(USDGENTONIC_ENABLE);
}

namespace {

TfToken const _scopeType("scope");
TfToken const _meshType("mesh");
TfToken const _curvesType("basisCurves");
TfToken const _pointsType("points");
TfToken const _materialType("material");

TfToken const _tokPoints("points");
TfToken const _tokNormals("normals");
TfToken const _tokWidths("widths");
TfToken const _tokDisplayColor("displayColor");
TfToken const _tokPrimvars("primvars");
TfToken const _tokPrimvarValue("primvarValue");
TfToken const _tokExtent("extent");
TfToken const _tokMin("min");
TfToken const _tokMax("max");
TfToken const _tokTopology("topology");
TfToken const _tokMesh("mesh");
TfToken const _tokBasisCurves("basisCurves");
TfToken const _tokVisibility("visibility");
TfToken const _tokTubeId("tubeId");
TfToken const _tokClumpColor("clumpColor");
TfToken const _tokSelected("selected");
TfToken const _tokXray("xray");
TfToken const _tokHierarchyLevel("hierarchyLevel");
TfToken const _tokCvIndex("cvIndex");
TfToken const _tokSectionIndex("sectionIndex");
TfToken const _tokHairT("hairT");
TfToken const _tokHandleId("handleId");
TfToken const _tokActive("active");
TfToken const _tokUnlit("unlit");
TfToken const _tokOpacity("opacity");
TfToken const _tokMaterialBindings("materialBindings");
TfToken const _tokConstant("constant");
TfToken const _tokUniform("uniform");
TfToken const _tokVertex("vertex");
TfToken const _tokColorRole("color");
TfToken const _tokPointRole("point");
TfToken const _tokNormalRole("normal");

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

HdContainerDataSourceHandle
_Primvar(HdSampledDataSourceHandle const &values,
         TfToken const &interpolation,
         TfToken const &role = TfToken())
{
    HdPrimvarSchema::Builder b;
    b.SetPrimvarValue(values);
    if (!interpolation.IsEmpty()) {
        b.SetInterpolation(_Tok(interpolation));
    }
    if (!role.IsEmpty()) {
        b.SetRole(_Tok(role));
    }
    return b.Build();
}

void
_Add(std::vector<TfToken> *names,
     std::vector<HdDataSourceBaseHandle> *values,
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
    if (names.empty()) {
        return HdRetainedContainerDataSource::New();
    }
    return HdRetainedContainerDataSource::New(
        names.size(), names.data(), values.data());
}

HdContainerDataSourceHandle
_IdentityXform()
{
    return HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(
            GfMatrix4d(1.0)))
        .SetResetXformStack(
            HdRetainedTypedSampledDataSource<bool>::New(true))
        .Build();
}

HdContainerDataSourceHandle
_Visible(bool visible)
{
    return HdVisibilitySchema::Builder()
        .SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(visible))
        .Build();
}

// A one-entry container that forces `visibility = false`. Overlaid on a
// tile prim it hides it without disturbing anything else the groom scene
// index published for it (plan/17 §3.2).
HdContainerDataSourceHandle
_HiddenOverlay()
{
    static HdContainerDataSourceHandle const hidden =
        HdRetainedContainerDataSource::New(_tokVisibility, _Visible(false));
    return hidden;
}

HdDataSourceBaseHandle
_Extent(GfVec3f const &mn, GfVec3f const &mx)
{
    return _Container({_tokMin, _tokMax},
                      {HdDataSourceBaseHandle(_Samp(mn)),
                       HdDataSourceBaseHandle(_Samp(mx))});
}

// HOW AN OVERLAY GETS DRAWN ON TOP (plan/18 §2.4a, V9).
//
// Centers, CV dots, rings, the gizmo and the brush ring all sit INSIDE or
// ON the tube they describe, so with an opaque tube in front of them they
// are simply not in the frame. Storm offers no per-prim "ignore the depth
// test", and the two things that look like it do not apply here:
//
//   * `displayInOverlay` (HdLegacyDisplayStyleSchema, and the material tag
//     HdStMaterialTagTokens->displayInOverlay it produces) is real, and
//     mesh/basisCurves/points all honour it — but HdxTaskController only
//     builds render tasks for defaultMaterialTag, masked, additive,
//     translucent and volume (hdx/taskController.cpp _CreateRenderGraph).
//     usdview uses that task controller, so a prim tagged displayInOverlay
//     lands in no collection at all and disappears instead of coming
//     forward. Same for `occludedSelectionShowsThrough`, whose
//     translucentToSelection tag only the shadow task knows.
//   * a depth bias in the shader would need the displacement terminal,
//     which Storm evaluates for meshes only (hdSt/shaders/mesh.glslfx is
//     the sole caller of DisplacementTerminal) — the overlays are curves
//     and points.
//
// What does work is to stop the OCCLUDER from writing depth. A material
// tagged `translucent` is drawn by HdxOitRenderTask after the opaque pass,
// composited out of the OIT buffers, and never writes to the depth buffer.
// So an x-rayed tube no longer hides anything: the overlays are ordinary
// opaque geometry, they draw first, and the ghosted tube blends over them.
//
// HdSt derives that tag from the material, and the strongest opinion below
// a hardcoded glslfx `materialTag` is the terminal's authored `opacity`
// (hdSt/materialNetwork.cpp _GetMaterialTag: "Weakest opinion is an
// authored terminal.opacity value" — anything under 1 means translucent).
// tonicTube.glslfx therefore no longer hardcodes a tag, and the index
// publishes the same shader twice: material_tube with opacity 1 (opaque
// pass) and material_tubeXray with opacity below 1 (OIT pass). A level's
// mesh binds whichever its x-ray state calls for.
//
// The number itself is never the alpha on screen. The alpha is the per-
// level `xray` primvar, so one x-ray material serves the focused level at
// 25 % and the levels behind it at 10 %; this constant exists only to put
// the draw items in the OIT pass.
float const _kXrayMaterialOpacity = 0.25f;

// One `surface` node under the universal render context (the tile
// publisher's _SurfaceMaterial pattern, plan/17 §4.4). `unlit` picks the
// glslfx's flat branch, which is what the overlay material is: one shader,
// two bindings, so the centers/rings/CV dots read as pure colour while the
// tubes are shaded.
HdContainerDataSourceHandle
_SurfaceMaterial(TfToken const &identifier,
                 std::vector<TfToken> const &paramNames,
                 std::vector<HdSampledDataSourceHandle> const &paramValues)
{
    static TfToken const surface("surface");
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    for (size_t i = 0; i < paramNames.size() && i < paramValues.size(); ++i) {
        names.push_back(paramNames[i]);
        values.push_back(HdMaterialNodeParameterSchema::Builder()
                             .SetValue(paramValues[i])
                             .Build());
    }
    HdContainerDataSourceHandle parameters =
        _Container(std::move(names), std::move(values));
    HdContainerDataSourceHandle const node =
        HdMaterialNodeSchema::Builder()
            .SetNodeIdentifier(_Tok(identifier))
            .SetParameters(parameters)
            .SetInputConnections(HdRetainedContainerDataSource::New())
            .Build();
    HdContainerDataSourceHandle const terminal =
        HdMaterialConnectionSchema::Builder()
            .SetUpstreamNodePath(_Tok(surface))
            .SetUpstreamNodeOutputName(_Tok(surface))
            .Build();
    HdContainerDataSourceHandle const network =
        HdMaterialNetworkSchema::Builder()
            .SetNodes(HdRetainedContainerDataSource::New(surface, node))
            .SetTerminals(
                HdRetainedContainerDataSource::New(surface, terminal))
            .Build();
    TfToken const contextName =
        HdMaterialSchemaTokens->universalRenderContext;
    HdDataSourceBaseHandle const contextValue = network;
    return HdRetainedContainerDataSource::New(
        HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1, &contextName, &contextValue));
}

// The guide preview's material: UsdGenHairPreview, parameterised so the
// strand keeps the clump colour it is published with (plan/18 §2.4a,
// "guides ... coloured by their clump").
//
// The shader's albedo is mix(root, tipColor, hairT^colorRamp) with `root`
// taken from the per-curve displayColor when the prim has one. With the
// stock defaults that is a lerp from the clump colour at the root to a
// fixed dark brown at the tip, tuned for rendered fur, so a Tonic guide
// arrived at the viewport as brown hair rather than as a coloured guide.
// Here the tip goes white instead, so the strand leaves its root in the
// clump colour and lightens along its length. That gradient is the point,
// not a decoration: a guide grown from a tube is drawn against that same
// tube, in that same clump colour, so a guide painted the flat clump
// colour is invisible — which is what the 2026-09-19 braid frame showed
// (the preview was published, drawn, and indistinguishable from the tube
// behind it). plan/18 §2.4a allows the lighter tint for exactly this
// reason. The diffuse gain and wrap are raised on the same grounds: this
// is an editing overlay that has to read under the viewer's default
// headlight, not a render of hair.
HdContainerDataSourceHandle
_GuidePreviewMaterial()
{
    return _SurfaceMaterial(
        TfToken("UsdGenHairPreview"),
        {TfToken("tipColor"), TfToken("colorRamp"), TfToken("diffuseGain"),
         TfToken("diffuseWrap"), TfToken("transmissionGain"),
         TfToken("specular1Gain")},
        {_Samp(GfVec3f(1.0f, 1.0f, 1.0f)), _Samp(1.2f), _Samp(0.90f),
         _Samp(0.70f), _Samp(0.10f), _Samp(0.12f)});
}

HdDataSourceBaseHandle
_MaterialBinding(SdfPath const &path)
{
    HdDataSourceBaseHandle binding =
        HdMaterialBindingSchema::Builder()
            .SetPath(
                HdRetainedTypedSampledDataSource<SdfPath>::New(path))
            .Build();
    return HdRetainedContainerDataSource::New(
        HdMaterialBindingsSchemaTokens->allPurpose, binding);
}

HdContainerDataSourceHandle
_MeshTopology(VtIntArray const &counts, VtIntArray const &indices)
{
    return HdMeshSchema::Builder()
        .SetTopology(HdMeshTopologySchema::Builder()
                         .SetFaceVertexCounts(
                             HdRetainedTypedSampledDataSource<VtIntArray>::
                                 New(counts))
                         .SetFaceVertexIndices(
                             HdRetainedTypedSampledDataSource<VtIntArray>::
                                 New(indices))
                         .SetOrientation(_Tok(HdTokens->rightHanded))
                         .Build())
        .SetSubdivisionScheme(_Tok(UsdGeomTokens->none))
        .SetDoubleSided(HdRetainedTypedSampledDataSource<bool>::New(true))
        .Build();
}

HdContainerDataSourceHandle
_CurvesTopology(VtIntArray const &counts, VtIntArray const &indices,
                TfToken const &type, TfToken const &basis,
                TfToken const &wrap)
{
    HdBasisCurvesTopologySchema::Builder b;
    b.SetCurveVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New(counts));
    b.SetCurveIndices(
        HdRetainedTypedSampledDataSource<VtIntArray>::New(indices));
    b.SetBasis(_Tok(basis));
    b.SetType(_Tok(type));
    b.SetWrap(_Tok(wrap));
    return HdBasisCurvesSchema::Builder().SetTopology(b.Build()).Build();
}

// The mesh data source for one level: topology, points/normals, the
// per-face identity primvars and the level's display state. Purpose is
// deliberately omitted (an absent purpose resolves to the geometry render
// tag; authoring purpose="default" would match no collection and Storm
// would never sync the prim — the 2026-09-12 tile lesson).
//
// tubeId/clumpColor/selected are UNIFORM, i.e. one value per face, expanded
// to the real face count. The P3 index published them as single-element
// `constant` arrays because it had one tube per mesh and a 1-element uniform
// on a 32-face mesh is rejected; a level mesh carries many tubes, so uniform
// is both the correct interpolation and correctly sized. The two the shader
// reads (clumpColor, selected) are floats: the repo's glslfx attributes only
// spell float/vec2/vec3 and the HD_HAS_-guarded HdGet_ reads need a matching
// type. tubeId and hierarchyLevel are ints — no shader reads them, only the
// tools and the tests.
HdContainerDataSourceHandle
_BuildLevelMeshDataSource(usdGenTonic::TonicStagedLevel const &level,
                          SdfPath const &materialPath)
{
    VtFloatArray selected(level.faceSelected.size());
    for (size_t i = 0; i < level.faceSelected.size(); ++i) {
        selected[i] = float(level.faceSelected[i]);
    }
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(level.points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokNormals,
         _Primvar(_Samp(level.normals), _tokVertex, _tokNormalRole));
    _Add(&pvNames, &pvValues, _tokTubeId,
         _Primvar(_Samp(level.faceTubeId), _tokUniform));
    _Add(&pvNames, &pvValues, _tokClumpColor,
         _Primvar(_Samp(level.faceClumpColor), _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokSelected,
         _Primvar(_Samp(selected), _tokUniform));
    _Add(&pvNames, &pvValues, _tokHierarchyLevel,
         _Primvar(_Samp(VtIntArray(1, level.level)), _tokConstant));
    // The ABSOLUTE alpha the level draws with, 0 when it is opaque. It was
    // a 0/1 flag through V8, with one `xrayOpacity` material parameter
    // behind it; plan/18 §2.4a needs the focused level and the levels
    // behind it ghosted by different amounts in the same frame, and a
    // primvar is the only one of the two that is per level.
    _Add(&pvNames, &pvValues, _tokXray,
         _Primvar(_Samp(VtFloatArray(1, level.xray ? level.xrayOpacity
                                                   : 0.0f)),
                  _tokConstant));

    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokMesh,
         _MeshTopology(level.faceVertexCounts, level.faceVertexIndices));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent,
         _Extent(level.extentMin, level.extentMax));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    // Centers-only keeps the center overlay and drops everything heavy:
    // the ladder's fourth step (plan/18 section 3.7).
    _Add(&names, &values, _tokVisibility,
         _Visible(level.visible && !level.centersOnly));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// One linear basisCurves overlay: center curves or section rings. Colour and
// width are uniform (per curve), so the focused level's curves thicken
// without touching a point.
HdContainerDataSourceHandle
_BuildCurvesDataSource(VtVec3fArray const &points, VtIntArray const &counts,
                       VtIntArray const &indices, VtVec3fArray const &colors,
                       VtFloatArray const &widths, VtIntArray const &tubeIds,
                       VtIntArray const &selected,
                       TfToken const &extraName, VtIntArray const &extra,
                       GfVec3f const &mn, GfVec3f const &mx, bool visible,
                       SdfPath const &materialPath)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(colors), _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokWidths,
         _Primvar(_Samp(widths), _tokUniform));
    _Add(&pvNames, &pvValues, _tokTubeId,
         _Primvar(_Samp(tubeIds), _tokUniform));
    if (!selected.empty()) {
        // FLOAT, exactly as the level mesh publishes it, and for the same
        // reason: `selected` is one of the three primvars tonicTube.glslfx
        // declares in its sdrMetadata, HdSt binds it for every prim that
        // carries the material, and the shader reads it as a float. The
        // mesh converted; the curves did not, and an int under a float
        // declaration shifts the rest of the uniform block -- which is
        // what drew one center curve pure green in the 2026-09-19 braid
        // frames, the first frames in which the curves were visible at
        // all (V9 x-ray). Nothing reads tubeId or sectionIndex in a
        // shader, so those stay ints.
        VtFloatArray asFloat(selected.size());
        for (size_t i = 0; i < selected.size(); ++i) {
            asFloat[i] = float(selected[i]);
        }
        _Add(&pvNames, &pvValues, _tokSelected,
             _Primvar(_Samp(asFloat), _tokUniform));
    }
    if (!extra.empty()) {
        _Add(&pvNames, &pvValues, extraName,
             _Primvar(_Samp(extra), _tokUniform));
    }
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokBasisCurves,
         _CurvesTopology(counts, indices, TfToken("linear"),
                         TfToken("bezier"), TfToken("nonperiodic")));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(visible));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// The CV dots: a points prim with per-vertex colour and width.
HdContainerDataSourceHandle
_BuildPointsDataSource(VtVec3fArray const &points, VtVec3fArray const &colors,
                       VtFloatArray const &widths, VtIntArray const &tubeIds,
                       TfToken const &extraName, VtIntArray const &extra,
                       GfVec3f const &mn, GfVec3f const &mx, bool visible,
                       SdfPath const &materialPath)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(colors), _tokVertex, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokWidths,
         _Primvar(_Samp(widths), _tokVertex));
    _Add(&pvNames, &pvValues, _tokTubeId,
         _Primvar(_Samp(tubeIds), _tokVertex));
    if (!extra.empty()) {
        _Add(&pvNames, &pvValues, extraName,
             _Primvar(_Samp(extra), _tokVertex));
    }
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(visible));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// The lowest refine level whose Storm repr carries the ribbon orientation
// vector UsdGenHairPreview reads (hdSt/basisCurves.cpp: 0 = WIRE lines,
// 1 = RIBBON + HAIR normal, 2 = RIBBON + ROUND). 2 is also what the tile
// pipeline treats as bound-and-shaded, so the preview matches it.
int const _kGuideRefineLevel = 2;

// The guide preview: basisCurves (cubic bspline, pinned — the committed
// Guides contract) with per-vertex widths + hairT for UsdGenHairPreview
// (variant A: patch reprs at refineLevel >= 2, the tile rule, 06 §4.1), plus
// the per-curve clump colour the 2014/2018 stills show.
HdContainerDataSourceHandle
_BuildGuideDataSource(usdGenTonic::TonicStagedLevel const &level,
                      SdfPath const &materialPath)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(level.guidePoints), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokWidths,
         _Primvar(_Samp(level.guideWidths), _tokVertex));
    _Add(&pvNames, &pvValues, _tokHairT,
         _Primvar(_Samp(level.guideHairT), _tokVertex));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(level.guideCurveColor), _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokTubeId,
         _Primvar(_Samp(level.guideCurveTubeId), _tokUniform));
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokBasisCurves,
         _CurvesTopology(level.guideVertexCounts, level.guideIndices,
                         TfToken("cubic"), TfToken("bspline"),
                         TfToken("pinned")));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(level.guideMin, level.guideMax));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    // The guide preview states its own refine level instead of taking the
    // host's fallback, because the material bound below cannot survive the
    // level the host would otherwise choose. UsdGenHairPreview reads the
    // ribbon orientation vector `inData.Neye`, and the WIRE repr Storm
    // draws at refineLevel 0 declares a curve vertex block that has no
    // Neye member (basisCurves.glslfx:1212-1218), so the shader fails to
    // COMPILE -- and Storm retries the compilation on every draw, which
    // turns the preview into hundreds of milliseconds per frame rather
    // than into a missing shade. usdGen's own tiles solve the same problem
    // at the other end, by hiding the binding below refineLevel 2
    // (groomSceneIndexPlugin.cpp DescriptionOverlay); this prim is an
    // overlay the tool owns, and the preview is only worth drawing shaded,
    // so it pins the level instead of dropping the shader.
    _Add(&names, &values, TfToken("displayStyle"),
         _Container({TfToken("refineLevel")},
                    {HdDataSourceBaseHandle(_Samp(_kGuideRefineLevel))}));
    // Centers-only keeps the center overlay and drops everything heavy:
    // the ladder's fourth step (plan/18 section 3.7). guidesVisible is the
    // other half of the "show amplified hair" swap (plan/17 section 3.2):
    // the preview steps aside for the cook's tiles.
    _Add(&names, &values, _tokVisibility,
         _Visible(level.visible && !level.centersOnly &&
                  level.guidesVisible));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// The gizmo and the brush ring (plan/18 §2.4): linear basisCurves with a
// per-curve colour, the handle id and whether that handle is the one being
// dragged. They bind the unlit overlay material, so what the artist sees is
// exactly the colour the record asked for, and they are ordinary prims, so
// they depth-sort against the tubes they manipulate.
HdContainerDataSourceHandle
_BuildOverlayDataSource(usdGenTonic::TonicOverlayCurves const &curves,
                        SdfPath const &materialPath)
{
    size_t const cvCount = curves.points.size() / 3;
    VtVec3fArray points(cvCount);
    VtIntArray indices(cvCount);
    for (size_t i = 0; i < cvCount; ++i) {
        points[i] = GfVec3f(curves.points[i * 3 + 0],
                            curves.points[i * 3 + 1],
                            curves.points[i * 3 + 2]);
        indices[i] = int(i);
    }
    size_t const curveCount = curves.vertexCounts.size();
    VtIntArray counts(curveCount);
    VtVec3fArray colors(curveCount);
    VtFloatArray widths(curveCount);
    VtIntArray handleIds(curveCount);
    VtIntArray active(curveCount);
    for (size_t c = 0; c < curveCount; ++c) {
        counts[c] = curves.vertexCounts[c];
        colors[c] = GfVec3f(curves.colors[c * 3 + 0],
                            curves.colors[c * 3 + 1],
                            curves.colors[c * 3 + 2]);
        widths[c] = curves.widths[c];
        handleIds[c] = curves.handleIds[c];
        active[c] = curves.active[c];
    }
    GfVec3f mn(0.0f), mx(0.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        if (i == 0) {
            mn = mx = points[0];
        } else {
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], points[i][a]);
                mx[a] = std::max(mx[a], points[i][a]);
            }
        }
    }
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(colors), _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokWidths,
         _Primvar(_Samp(widths), _tokUniform));
    _Add(&pvNames, &pvValues, _tokHandleId,
         _Primvar(_Samp(handleIds), _tokUniform));
    _Add(&pvNames, &pvValues, _tokActive,
         _Primvar(_Samp(active), _tokUniform));
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokBasisCurves,
         _CurvesTopology(counts, indices, TfToken("linear"),
                         TfToken("bezier"), TfToken("nonperiodic")));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(true));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// The P0 test tube, published while no model is active.
HdContainerDataSourceHandle
_BuildTestTubeDataSource(usdGenTonic::TonicStagedTubeMesh const &tube,
                         SdfPath const &materialPath)
{
    usdGenTonic::TonicRgb const rgb = usdGenTonic::TonicClumpColor(0, 1, -1);
    size_t const faceCount = tube.faceVertexCounts.size();
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(tube.points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokNormals,
         _Primvar(_Samp(tube.normals), _tokVertex, _tokNormalRole));
    _Add(&pvNames, &pvValues, _tokTubeId,
         _Primvar(_Samp(VtIntArray(faceCount, 0)), _tokUniform));
    _Add(&pvNames, &pvValues, _tokClumpColor,
         _Primvar(_Samp(VtVec3fArray(faceCount,
                                     GfVec3f(rgb.r, rgb.g, rgb.b))),
                  _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokSelected,
         _Primvar(_Samp(VtFloatArray(faceCount, 0.0f)), _tokUniform));
    _Add(&pvNames, &pvValues, _tokHierarchyLevel,
         _Primvar(_Samp(VtIntArray(1, 1)), _tokConstant));
    _Add(&pvNames, &pvValues, _tokXray,
         _Primvar(_Samp(VtFloatArray(1, 0.0f)), _tokConstant));
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokMesh,
         _MeshTopology(tube.faceVertexCounts, tube.faceVertexIndices));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(tube.extentMin, tube.extentMax));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(true));
    if (!materialPath.IsEmpty()) {
        _Add(&names, &values, _tokMaterialBindings,
             _MaterialBinding(materialPath));
    }
    return _Container(std::move(names), std::move(values));
}

// Graph nodes as a points prim: positions + per-point display colour
// (white; unwelded coincident nodes warn orange — the HUD ring's data).
HdContainerDataSourceHandle
_BuildNodesDataSource(VtVec3fArray const &points, VtVec3fArray const &colors,
                      VtFloatArray const &widths)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(colors), _tokVertex, _tokColorRole));
    _Add(&pvNames, &pvValues, _tokWidths,
         _Primvar(_Samp(widths), _tokVertex));
    GfVec3f mn(0.0f), mx(0.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        if (i == 0) {
            mn = mx = points[0];
        } else {
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], points[i][a]);
                mx[a] = std::max(mx[a], points[i][a]);
            }
        }
    }
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(true));
    return _Container(std::move(names), std::move(values));
}

// Graph edges as linear basisCurves: one curve per edge polyline, uniform
// per-curve colour (shared edges white, border edges grey — plan/17 §5.1).
HdContainerDataSourceHandle
_BuildEdgesDataSource(VtIntArray const &curveVertexCounts,
                      VtIntArray const &curveIndices,
                      VtVec3fArray const &points,
                      VtVec3fArray const &curveColors)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(curveColors), _tokUniform, _tokColorRole));
    GfVec3f mn(0.0f), mx(0.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        if (i == 0) {
            mn = mx = points[0];
        } else {
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], points[i][a]);
                mx[a] = std::max(mx[a], points[i][a]);
            }
        }
    }
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokBasisCurves,
         _CurvesTopology(curveVertexCounts, curveIndices, TfToken("linear"),
                         TfToken("bezier"), TfToken("nonperiodic")));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(true));
    return _Container(std::move(names), std::move(values));
}

// Live region tint: the scalp topology with per-face display colours and
// the tonicRegion primvar (plan/17 §5.1 HUD overlays). The colours come from
// the same palette the tubes use, so the patch on the head and the tube
// rooted in it match (plan/18 §2.4a).
HdContainerDataSourceHandle
_BuildRegionsDataSource(VtVec3fArray const &points,
                        VtIntArray const &faceVertexCounts,
                        VtIntArray const &faceVertexIndices,
                        VtVec3fArray const &faceColors,
                        VtIntArray const &faceRegions)
{
    std::vector<TfToken> pvNames;
    std::vector<HdDataSourceBaseHandle> pvValues;
    _Add(&pvNames, &pvValues, _tokPoints,
         _Primvar(_Samp(points), _tokVertex, _tokPointRole));
    _Add(&pvNames, &pvValues, _tokDisplayColor,
         _Primvar(_Samp(faceColors), _tokUniform, _tokColorRole));
    _Add(&pvNames, &pvValues, TfToken("usdGen:tonicRegion"),
         _Primvar(_Samp(faceRegions), _tokUniform));
    GfVec3f mn(0.0f), mx(0.0f);
    for (size_t i = 0; i < points.size(); ++i) {
        if (i == 0) {
            mn = mx = points[0];
        } else {
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], points[i][a]);
                mx[a] = std::max(mx[a], points[i][a]);
            }
        }
    }
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    _Add(&names, &values, _tokMesh,
         _MeshTopology(faceVertexCounts, faceVertexIndices));
    _Add(&names, &values, _tokPrimvars,
         _Container(std::move(pvNames), std::move(pvValues)));
    _Add(&names, &values, _tokExtent, _Extent(mn, mx));
    _Add(&names, &values, TfToken("xform"), _IdentityXform());
    _Add(&names, &values, _tokVisibility, _Visible(true));
    return _Container(std::move(names), std::move(values));
}

SdfPath _LevelPath(SdfPath const &scope, int level)
{
    return scope.AppendChild(TfToken("L" + std::to_string(level)));
}

} // namespace

// -- paths --------------------------------------------------------------------

SdfPath const &
UsdGenTonicSceneIndex::RootPath()
{
    static SdfPath const root("/__usdGenTonic");
    return root;
}

SdfPath const &
UsdGenTonicSceneIndex::TestTubePath()
{
    static SdfPath const tube("/__usdGenTonic/testTube");
    return tube;
}

SdfPath const &
UsdGenTonicSceneIndex::TubesScopePath()
{
    static SdfPath const p("/__usdGenTonic/tubes");
    return p;
}

SdfPath const &
UsdGenTonicSceneIndex::CentersScopePath()
{
    static SdfPath const p("/__usdGenTonic/centers");
    return p;
}

SdfPath const &
UsdGenTonicSceneIndex::CenterCVsScopePath()
{
    static SdfPath const p("/__usdGenTonic/centerCVs");
    return p;
}

SdfPath const &
UsdGenTonicSceneIndex::RingsScopePath()
{
    static SdfPath const p("/__usdGenTonic/rings");
    return p;
}

SdfPath const &
UsdGenTonicSceneIndex::RingCVsScopePath()
{
    static SdfPath const p("/__usdGenTonic/ringCVs");
    return p;
}

SdfPath const &
UsdGenTonicSceneIndex::GuidesScopePath()
{
    static SdfPath const p("/__usdGenTonic/guides");
    return p;
}

SdfPath UsdGenTonicSceneIndex::TubesPath(int level)
{
    return _LevelPath(TubesScopePath(), level);
}

SdfPath UsdGenTonicSceneIndex::CentersPath(int level)
{
    return _LevelPath(CentersScopePath(), level);
}

SdfPath UsdGenTonicSceneIndex::CenterCVsPath(int level)
{
    return _LevelPath(CenterCVsScopePath(), level);
}

SdfPath UsdGenTonicSceneIndex::RingsPath(int level)
{
    return _LevelPath(RingsScopePath(), level);
}

SdfPath UsdGenTonicSceneIndex::RingCVsPath(int level)
{
    return _LevelPath(RingCVsScopePath(), level);
}

SdfPath UsdGenTonicSceneIndex::GuidesPath(int level)
{
    return _LevelPath(GuidesScopePath(), level);
}

SdfPath const &
UsdGenTonicSceneIndex::GraphNodesPath()
{
    static SdfPath const nodes("/__usdGenTonic/graphNodes");
    return nodes;
}

SdfPath const &
UsdGenTonicSceneIndex::GraphEdgesPath()
{
    static SdfPath const edges("/__usdGenTonic/graphEdges");
    return edges;
}

SdfPath const &
UsdGenTonicSceneIndex::GraphRegionsPath()
{
    static SdfPath const regions("/__usdGenTonic/graphRegions");
    return regions;
}

SdfPath const &
UsdGenTonicSceneIndex::GizmoPath()
{
    static SdfPath const gizmo("/__usdGenTonic/gizmo");
    return gizmo;
}

SdfPath const &
UsdGenTonicSceneIndex::BrushRingPath()
{
    static SdfPath const brush("/__usdGenTonic/brushRing");
    return brush;
}

SdfPath const &
UsdGenTonicSceneIndex::TubeMaterialPath()
{
    static SdfPath const material("/__usdGenTonic/material_tube");
    return material;
}

SdfPath const &
UsdGenTonicSceneIndex::TubeXrayMaterialPath()
{
    static SdfPath const material("/__usdGenTonic/material_tubeXray");
    return material;
}

SdfPath const &
UsdGenTonicSceneIndex::HairMaterialPath()
{
    static SdfPath const material("/__usdGenTonic/material_hairPreview");
    return material;
}

SdfPath const &
UsdGenTonicSceneIndex::OverlayMaterialPath()
{
    static SdfPath const material("/__usdGenTonic/material_overlay");
    return material;
}

// -- lifetime -----------------------------------------------------------------

HdSceneIndexBaseRefPtr
UsdGenTonicSceneIndex::New(HdSceneIndexBaseRefPtr const &inputScene)
{
    return TfCreateRefPtr(new UsdGenTonicSceneIndex(inputScene));
}

UsdGenTonicSceneIndex::UsdGenTonicSceneIndex(
    HdSceneIndexBaseRefPtr const &inputScene)
    : HdSingleInputFilteringSceneIndexBase(inputScene)
    , _publisher(new usdGenTonic::TonicPublisher())
    , _published(new usdGenTonic::TonicStagedModel())
{
    // The root scope always exists, so GetChildPrimPaths can be a single
    // walk of the prim table.
    _prims[RootPath()] = _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    usdGenTonic::TonicRegistry::Get().AttachIndex(this);
    // Publish whatever is already active (usually nothing, and then the
    // test tube unless a headless record opted out).
    Refresh(~0u);
}

UsdGenTonicSceneIndex::~UsdGenTonicSceneIndex()
{
    usdGenTonic::TonicRegistry::Get().DetachIndex(this);
}

// -- queries ------------------------------------------------------------------

HdSceneIndexPrim
UsdGenTonicSceneIndex::GetPrim(SdfPath const &primPath) const
{
    SdfPath hiddenGuides;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _prims.find(primPath);
        if (it != _prims.end()) {
            return {it->second.primType, it->second.dataSource};
        }
        hiddenGuides = _hiddenGuidesPath;
    }
    if (primPath.HasPrefix(RootPath())) {
        // Our namespace, nothing published there: never fall through to the
        // input, which does not own /__usdGenTonic (plan/17 R5).
        return {TfToken(), nullptr};
    }
    HdSceneIndexBaseRefPtr const &input = _GetInputSceneIndex();
    if (!input) {
        return {TfToken(), nullptr};
    }
    HdSceneIndexPrim prim = input->GetPrim(primPath);
    // plan/17 section 3.2: the usdGen cook's amplified tiles are hidden
    // while a gesture is live (they carry a stage version older than the
    // drag) and whenever "show amplified hair" is off. The override is an
    // overlay, so the tile keeps every other data source the groom index
    // published and comes straight back when the flag flips.
    if (prim.dataSource && !_amplifiedTilesVisible.load() &&
        _IsAmplifiedTilePath(primPath)) {
        prim.dataSource = HdOverlayContainerDataSource::New(
            _HiddenOverlay(), prim.dataSource);
    }
    // plan/18 §2.4a: while a model is live, the committed <groom>/Guides
    // prim is the PREVIOUS commit's curves. It is an ordinary BasisCurves
    // with no displayColor, so Storm draws it plain white on top of every
    // tube the tool is publishing — which is exactly what the 2026-09-18
    // workspace frame showed. Hiding it is a Hydra opinion and nothing
    // more: the amplifier reads the stage, not this index, so the cook
    // and its tiles are untouched. Deactivating the model clears the path
    // and the prim comes straight back.
    if (prim.dataSource && !hiddenGuides.IsEmpty() &&
        primPath.HasPrefix(hiddenGuides)) {
        prim.dataSource = HdOverlayContainerDataSource::New(
            _HiddenOverlay(), prim.dataSource);
    }
    return prim;
}

bool
UsdGenTonicSceneIndex::_IsAmplifiedTilePath(SdfPath const &path)
{
    static TfToken const renderScope("__usdGenRender");
    for (SdfPath p = path; !p.IsEmpty() && p != SdfPath::AbsoluteRootPath();
         p = p.GetParentPath()) {
        if (p.GetNameToken() == renderScope) {
            return true;
        }
    }
    return false;
}

SdfPathVector
UsdGenTonicSceneIndex::GetChildPrimPaths(SdfPath const &path) const
{
    // The tonic subtree is additive: input children pass through untouched.
    SdfPathVector children;
    if (!path.HasPrefix(RootPath()) || path == SdfPath::AbsoluteRootPath()) {
        HdSceneIndexBaseRefPtr const &input = _GetInputSceneIndex();
        if (input) {
            children = input->GetChildPrimPaths(path);
        }
    }
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto const &kv : _prims) {
        if (kv.first.GetParentPath() == path) {
            children.push_back(kv.first);
        }
    }
    return children;
}

std::vector<int>
UsdGenTonicSceneIndex::PublishedLevels() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> levels;
    for (auto const &kv : _published->levels) {
        levels.push_back(kv.first);
    }
    return levels;
}

bool
UsdGenTonicSceneIndex::PublishedLevelInfo(int level, int *outFaceCount,
                                          int *outPointCount,
                                          int *outTubeCount) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _published->levels.find(level);
    if (it == _published->levels.end()) {
        return false;
    }
    if (outFaceCount) {
        *outFaceCount = int(it->second.faceVertexCounts.size());
    }
    if (outPointCount) {
        *outPointCount = int(it->second.points.size());
    }
    if (outTubeCount) {
        *outTubeCount = int(it->second.tubes.size());
    }
    return true;
}

bool
UsdGenTonicSceneIndex::QueryPublishedLevel(int level, int *outFaceCount,
                                           int *outPointCount,
                                           int *outTubeCount) const
{
    return PublishedLevelInfo(level, outFaceCount, outPointCount,
                              outTubeCount);
}

int
UsdGenTonicSceneIndex::LastRestagedTubeCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lastRestagedTubeCount;
}

bool
UsdGenTonicSceneIndex::HasTestTube() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _testTubePublished;
}

// -- notices ------------------------------------------------------------------

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::NoticesFor(uint32_t dirty)
{
    HdDataSourceLocatorSet locators;
    if (dirty & usdGenTonic::TonicDirty_Topology) {
        locators.insert(HdDataSourceLocator(_tokMesh, _tokTopology));
    }
    if (dirty & (usdGenTonic::TonicDirty_Points |
                 usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokNormals, _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    }
    if (dirty & (Dirty_Uniforms | usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokTubeId, _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokClumpColor,
                                            _tokPrimvarValue));
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokSelected, _tokPrimvarValue));
    }
    if (dirty & usdGenTonic::TonicDirty_Selection) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokSelected, _tokPrimvarValue));
    }
    if (dirty & Dirty_Xray) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokXray, _tokPrimvarValue));
    }
    if (dirty & Dirty_Material) {
        // The level swapped between the opaque and the translucent twin of
        // the tube shader, which is a different binding, not a different
        // parameter (see _kXrayMaterialOpacity).
        locators.insert(HdDataSourceLocator(_tokMaterialBindings));
    }
    if (dirty & Dirty_Visibility) {
        locators.insert(HdDataSourceLocator(_tokVisibility, _tokVisibility));
    }
    return locators;
}

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::CurveNoticesFor(uint32_t dirty)
{
    HdDataSourceLocatorSet locators;
    if (dirty & usdGenTonic::TonicDirty_Topology) {
        locators.insert(HdDataSourceLocator(_tokBasisCurves, _tokTopology));
    }
    if (dirty & (usdGenTonic::TonicDirty_Points |
                 usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    }
    if (dirty & (Dirty_Uniforms | usdGenTonic::TonicDirty_Topology)) {
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokTubeId, _tokPrimvarValue));
    }
    if (dirty & (Dirty_Widths | usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokWidths, _tokPrimvarValue));
    }
    if (dirty & usdGenTonic::TonicDirty_Selection) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokSelected, _tokPrimvarValue));
    }
    if (dirty & Dirty_Visibility) {
        locators.insert(HdDataSourceLocator(_tokVisibility, _tokVisibility));
    }
    return locators;
}

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::PointNoticesFor(uint32_t dirty)
{
    HdDataSourceLocatorSet locators;
    if (dirty & (usdGenTonic::TonicDirty_Points |
                 usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    }
    if (dirty & (Dirty_Uniforms | usdGenTonic::TonicDirty_Topology)) {
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokTubeId, _tokPrimvarValue));
    }
    if (dirty & (Dirty_Widths | usdGenTonic::TonicDirty_Topology)) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokWidths, _tokPrimvarValue));
    }
    if (dirty & usdGenTonic::TonicDirty_Selection) {
        // A CV dot carries its state in its colour (white selected, yellow
        // hovered), so selection dirties displayColor and nothing else.
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
    }
    if (dirty & Dirty_Visibility) {
        locators.insert(HdDataSourceLocator(_tokVisibility, _tokVisibility));
    }
    return locators;
}

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::GuideNoticesFor(uint32_t dirty, bool countChanged)
{
    HdDataSourceLocatorSet locators;
    if (dirty & Dirty_Visibility) {
        locators.insert(HdDataSourceLocator(_tokVisibility, _tokVisibility));
    }
    if (!(dirty & usdGenTonic::TonicDirty_Guides)) {
        return locators;
    }
    if (countChanged) {
        locators.insert(HdDataSourceLocator(_tokBasisCurves, _tokTopology));
    }
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokWidths, _tokPrimvarValue));
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokHairT, _tokPrimvarValue));
    locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
    locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    return locators;
}

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::GraphNoticesFor(uint32_t dirty)
{
    // One locator set per overlay prim would be most precise; the graph and
    // the tint restage together (both derive from one snapshot), so one set
    // covers the overlay: nodes/edges points + topology on graph edits, the
    // tint leaves on region edits. Each entry below is addressed to its own
    // prim by the Refresh fan-out.
    HdDataSourceLocatorSet locators;
    if (dirty & usdGenTonic::TonicDirty_Graph) {
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(_tokBasisCurves, _tokTopology));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
        locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    }
    if (dirty & usdGenTonic::TonicDirty_Regions) {
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
        locators.insert(HdDataSourceLocator(
            _tokPrimvars, TfToken("usdGen:tonicRegion"), _tokPrimvarValue));
    }
    if (dirty & usdGenTonic::TonicDirty_Selection) {
        locators.insert(HdDataSourceLocator(_tokPrimvars, _tokDisplayColor,
                                            _tokPrimvarValue));
    }
    return locators;
}

HdDataSourceLocatorSet
UsdGenTonicSceneIndex::OverlayNoticesFor(uint32_t dirty, bool countChanged)
{
    HdDataSourceLocatorSet locators;
    if (!(dirty & (usdGenTonic::TonicDirty_Gizmo |
                   usdGenTonic::TonicDirty_Brush))) {
        return locators;
    }
    if (countChanged) {
        locators.insert(HdDataSourceLocator(_tokBasisCurves, _tokTopology));
        locators.insert(
            HdDataSourceLocator(_tokPrimvars, _tokHandleId, _tokPrimvarValue));
    }
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokPoints, _tokPrimvarValue));
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokDisplayColor, _tokPrimvarValue));
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokWidths, _tokPrimvarValue));
    locators.insert(
        HdDataSourceLocator(_tokPrimvars, _tokActive, _tokPrimvarValue));
    locators.insert(HdDataSourceLocator(_tokExtent, _tokMin));
    locators.insert(HdDataSourceLocator(_tokExtent, _tokMax));
    return locators;
}

// -- publication --------------------------------------------------------------

void
UsdGenTonicSceneIndex::_BuildTestTubePrims(
    std::map<SdfPath, _Prim> *prims) const
{
    usdGenTonic::TonicStagedTubeMesh tube;
    usdGenTonic::TonicStageTestTubeMesh(usdGenTonic::TonicTubeShape(), &tube);
    (*prims)[TestTubePath()] =
        _Prim{_meshType, _BuildTestTubeDataSource(tube, TubeMaterialPath())};
}

void
UsdGenTonicSceneIndex::_BuildLevelPrims(
    usdGenTonic::TonicStagedModel const &staged,
    std::map<SdfPath, _Prim> *prims) const
{
    if (staged.levels.empty()) {
        return;
    }
    (*prims)[TubesScopePath()] =
        _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    (*prims)[CentersScopePath()] =
        _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    (*prims)[CenterCVsScopePath()] =
        _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    (*prims)[RingsScopePath()] =
        _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    (*prims)[RingCVsScopePath()] =
        _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    for (auto const &kv : staged.levels) {
        int const level = kv.first;
        usdGenTonic::TonicStagedLevel const &staging = kv.second;
        // An x-rayed level binds the translucent twin so it stops writing
        // depth and the overlays inside it come through (see
        // _kXrayMaterialOpacity).
        (*prims)[TubesPath(level)] =
            _Prim{_meshType,
                  _BuildLevelMeshDataSource(
                      staging, staging.xray ? TubeXrayMaterialPath()
                                            : TubeMaterialPath())};
        (*prims)[CentersPath(level)] =
            _Prim{_curvesType,
                  _BuildCurvesDataSource(
                      staging.centerPoints, staging.centerVertexCounts,
                      staging.centerIndices, staging.centerCurveColor,
                      staging.centerCurveWidth, staging.centerCurveTubeId,
                      staging.centerCurveSelected, TfToken(), VtIntArray(),
                      staging.centerMin, staging.centerMax,
                      staging.visible && staging.centers,
                      OverlayMaterialPath())};
        (*prims)[CenterCVsPath(level)] =
            _Prim{_pointsType,
                  _BuildPointsDataSource(
                      staging.centerPoints, staging.centerCVColor,
                      staging.centerCVWidth, staging.centerCVTubeId,
                      _tokCvIndex, staging.centerCVIndex, staging.centerMin,
                      staging.centerMax, staging.visible && staging.centers,
                      OverlayMaterialPath())};
        (*prims)[RingsPath(level)] =
            _Prim{_curvesType,
                  _BuildCurvesDataSource(
                      staging.ringPoints, staging.ringVertexCounts,
                      staging.ringIndices, staging.ringCurveColor,
                      staging.ringCurveWidth, staging.ringCurveTubeId,
                      staging.ringCurveSelected, _tokSectionIndex,
                      staging.ringCurveSection,
                      staging.ringMin, staging.ringMax,
                      staging.visible && staging.centers &&
                          !staging.centersOnly &&
                          !staging.ringPoints.empty(),
                      OverlayMaterialPath())};
        (*prims)[RingCVsPath(level)] =
            _Prim{_pointsType,
                  _BuildPointsDataSource(
                      staging.ringCVPoints, staging.ringCVColor,
                      staging.ringCVWidth, staging.ringCVTubeId,
                      _tokSectionIndex, staging.ringCVSection, staging.ringMin,
                      staging.ringMax,
                      staging.visible && staging.centers &&
                          !staging.centersOnly &&
                          !staging.ringCVPoints.empty(),
                      OverlayMaterialPath())};
        if (staging.guideCount > 0 && !staging.guidePoints.empty()) {
            (*prims)[GuidesScopePath()] =
                _Prim{_scopeType, HdRetainedContainerDataSource::New()};
            (*prims)[GuidesPath(level)] =
                _Prim{_curvesType,
                      _BuildGuideDataSource(staging, HairMaterialPath())};
        }
    }
}

void
UsdGenTonicSceneIndex::_BuildGraphPrims(usdGenTonic::TonicModel &model,
                                        std::map<SdfPath, _Prim> *prims) const
{
    if (!model.HasScalp()) {
        return;
    }
    usdGenTonic::TonicModel::GraphSnapshot snap = model.SnapshotGraph();
    std::shared_ptr<usdGenTonic::TonicScalpMesh const> scalp =
        model.GetScalp();
    // Selected graph items draw white, the hovered one yellow (plan/18
    // §2.4a); the palette matches the CV dots so one rule reads across
    // the whole overlay.
    static GfVec3f const kSelected(1.0f, 1.0f, 1.0f);
    static GfVec3f const kHover(1.0f, 0.85f, 0.10f);
    std::set<int> selectedNodes;
    std::set<int> selectedEdges;
    for (usdGenTonic::TonicSelectionItem const &item :
         model.SelectionItems(usdGenTonic::TonicPick_GraphNode |
                              usdGenTonic::TonicPick_GraphEdge)) {
        if (item.kind == usdGenTonic::TonicPick_GraphNode) {
            selectedNodes.insert(item.id);
        } else {
            selectedEdges.insert(item.id);
        }
    }
    usdGenTonic::TonicSelectionItem const hover = model.SelectionHover();
    // Nodes: white, coincident (unwelded) sets warn orange, selected
    // white-bright and the hovered one yellow.
    VtVec3fArray nodePoints;
    VtVec3fArray nodeColors;
    nodePoints.reserve(snap.nodes.size());
    nodeColors.reserve(snap.nodes.size());
    std::set<int> coincident;
    for (auto const &group :
         model.GetGraph().CoincidentSets(model.GetSnapRadius())) {
        coincident.insert(group.begin(), group.end());
    }
    for (auto const &nd : snap.nodes) {
        nodePoints.push_back(GfVec3f(nd.p[0], nd.p[1], nd.p[2]));
        bool const hovered =
            hover.kind == usdGenTonic::TonicPick_GraphNode &&
            hover.id == nd.id;
        if (hovered) {
            nodeColors.push_back(kHover);
        } else if (selectedNodes.count(nd.id)) {
            nodeColors.push_back(kSelected);
        } else {
            nodeColors.push_back(coincident.count(nd.id)
                                     ? GfVec3f(1.0f, 0.5f, 0.1f)
                                     : GfVec3f(0.80f));
        }
    }
    // Edges: one linear curve per polyline; shared white, border grey.
    VtIntArray curveCounts;
    VtIntArray curveIndices;
    VtVec3fArray edgePoints;
    VtVec3fArray edgeColors;
    {
        int cursor = 0;
        for (auto const &e : model.GetGraph().Edges()) {
            if (!e.alive || e.polyline.size() < 6) {
                continue;
            }
            int const n = int(e.polyline.size() / 3);
            curveCounts.push_back(n);
            for (int i = 0; i < n; ++i) {
                curveIndices.push_back(cursor + i);
                edgePoints.push_back(GfVec3f(e.polyline[size_t(i) * 3 + 0],
                                             e.polyline[size_t(i) * 3 + 1],
                                             e.polyline[size_t(i) * 3 + 2]));
            }
            cursor += n;
            size_t const sides = model.GetGraph().EdgeRegions(e.id).size();
            bool const hovered =
                hover.kind == usdGenTonic::TonicPick_GraphEdge &&
                hover.id == e.id;
            if (hovered) {
                edgeColors.push_back(kHover);
            } else if (selectedEdges.count(e.id)) {
                edgeColors.push_back(kSelected);
            } else {
                edgeColors.push_back(sides >= 2 ? GfVec3f(0.85f)
                                                : GfVec3f(0.45f));
            }
        }
    }
    // Tint mesh: scalp topology + per-face region colours from the clump
    // palette, so the scalp patch and the tube rooted in it are the same
    // colour (plan/18 §2.4a). Uncovered is dark red, intersected magenta.
    //
    // The colour is keyed on the face's REGION id (`faceRegionIds`, the
    // lowest region claiming the face), which is the id the L1 tube rooted
    // there carries in its desc, so the two palette lookups are the same
    // lookup. Keying it on the face's interp id — the union-find channel
    // the bake writes — was the V0..V7 spelling and is not the same thing:
    // linked regions share an interp id but keep their own region ids and
    // their own tubes, so the patch took whichever member the map happened
    // to see first while the tubes each took their own colour.
    //
    // The mesh is also pushed off the scalp along its own vertex normals.
    // It carries the scalp's points verbatim otherwise, and two coincident
    // opaque meshes resolve by depth-buffer luck: the golden braid frame
    // showed the tint winning 36 % of the scalp's pixels and the bound
    // mesh the rest, and in the workspace shot the tint lost everywhere,
    // so the artist saw an untinted head. The offset is a fixed fraction
    // of the scalp's bounding diagonal, which puts it two orders of
    // magnitude above the depth resolution at any sane framing while
    // staying under a pixel or two of parallax.
    VtVec3fArray scalpPoints;
    VtIntArray scalpCounts;
    VtIntArray scalpIndices;
    VtVec3fArray faceColors;
    VtIntArray faceRegions;
    if (scalp && scalp->finalized) {
        size_t const vertexCount = scalp->points.size() / 3;
        scalpCounts.assign(scalp->faceVertexCounts.begin(),
                           scalp->faceVertexCounts.end());
        scalpIndices.assign(scalp->faceVertexIndices.begin(),
                            scalp->faceVertexIndices.end());
        scalpPoints.assign(vertexCount, GfVec3f(0.0f));
        std::vector<GfVec3f> vertexNormal(vertexCount, GfVec3f(0.0f));
        for (size_t f = 0; f + 1 < scalp->faceOffsets.size(); ++f) {
            if (f * 3 + 2 >= scalp->faceNormals.size()) {
                break;
            }
            GfVec3f const n(scalp->faceNormals[f * 3 + 0],
                            scalp->faceNormals[f * 3 + 1],
                            scalp->faceNormals[f * 3 + 2]);
            for (int k = scalp->faceOffsets[f]; k < scalp->faceOffsets[f + 1];
                 ++k) {
                int const v = scalp->faceVertexIndices[size_t(k)];
                if (v >= 0 && size_t(v) < vertexCount) {
                    vertexNormal[size_t(v)] += n;
                }
            }
        }
        GfVec3f lo(0.0f), hi(0.0f);
        for (size_t i = 0; i < vertexCount; ++i) {
            GfVec3f const p(scalp->points[i * 3 + 0], scalp->points[i * 3 + 1],
                            scalp->points[i * 3 + 2]);
            scalpPoints[i] = p;
            if (i == 0) {
                lo = hi = p;
            } else {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], p[a]);
                    hi[a] = std::max(hi[a], p[a]);
                }
            }
        }
        float const offset = 2.0e-3f * (hi - lo).GetLength();
        if (offset > 0.0f) {
            for (size_t i = 0; i < vertexCount; ++i) {
                GfVec3f n = vertexNormal[i];
                if (n.GetLengthSq() > 1e-20f) {
                    scalpPoints[i] += n.GetNormalized() * offset;
                }
            }
        }
        usdGenTonic::TonicRegionMaps const &maps = model.GetRegionMaps();
        for (size_t f = 0; f < snap.faceRegions.size(); ++f) {
            int const interp = snap.faceRegions[f];
            faceRegions.push_back(interp);
            int const regionId = f < snap.faceRegionIds.size()
                                     ? snap.faceRegionIds[f]
                                     : -1;
            if (interp < 0 || regionId < 0) {
                faceColors.push_back(GfVec3f(0.35f, 0.05f, 0.05f));
            } else if (f < maps.intersected.size() && maps.intersected[f]) {
                faceColors.push_back(GfVec3f(1.0f, 0.0f, 1.0f));
            } else {
                usdGenTonic::TonicRgb const rgb =
                    usdGenTonic::TonicClumpColor(regionId, 1, -1);
                faceColors.push_back(GfVec3f(rgb.r, rgb.g, rgb.b));
            }
        }
    }
    if (!nodePoints.empty()) {
        // Node dots are a fixed pixel size like every other overlay dot
        // (plan/18 §2.4a); before V8 they published no `widths` at all and
        // took whatever Storm's default point size happened to be. With no
        // camera resolved they fall back to a fraction of the scalp's mean
        // edge, which is the only length the graph knows about itself.
        float const nodeWidth = usdGenTonic::TonicOverlayWidth(
            usdGenTonic::TonicOverlayPixels::kGraphNode,
            model.GetDisplayScale(),
            scalp && scalp->meanEdgeLength > 0.0f
                ? scalp->meanEdgeLength * 0.12f
                : 0.05f);
        (*prims)[GraphNodesPath()] =
            _Prim{_pointsType,
                  _BuildNodesDataSource(nodePoints, nodeColors,
                                        VtFloatArray(nodePoints.size(),
                                                     nodeWidth))};
    }
    if (!edgePoints.empty()) {
        (*prims)[GraphEdgesPath()] =
            _Prim{_curvesType, _BuildEdgesDataSource(curveCounts, curveIndices,
                                                     edgePoints, edgeColors)};
    }
    if (!scalpPoints.empty()) {
        (*prims)[GraphRegionsPath()] =
            _Prim{_meshType,
                  _BuildRegionsDataSource(scalpPoints, scalpCounts,
                                          scalpIndices, faceColors,
                                          faceRegions)};
    }
}

UsdGenTonicSceneIndex::_OverlayDirty
UsdGenTonicSceneIndex::_BuildOverlayPrims(usdGenTonic::TonicModel &model,
                                          std::map<SdfPath, _Prim> *prims)
{
    _OverlayDirty dirty;
    usdGenTonic::TonicGizmoRecord const gizmo = model.GetGizmo();
    usdGenTonic::TonicBrushRingRecord const brush = model.GetBrushRing();
    usdGenTonic::TonicOverlayCurves curves;
    if (usdGenTonic::TonicBuildGizmoCurves(gizmo, &curves)) {
        (*prims)[GizmoPath()] =
            _Prim{_curvesType,
                  _BuildOverlayDataSource(curves, OverlayMaterialPath())};
        if (gizmo != _publishedGizmo) {
            dirty.gizmo = usdGenTonic::TonicDirty_Gizmo;
        }
        dirty.gizmoCountChanged =
            curves.CurveCount() != _publishedGizmoCurves;
        _publishedGizmoCurves = curves.CurveCount();
    } else {
        _publishedGizmoCurves = 0;
    }
    if (usdGenTonic::TonicBuildBrushRingCurves(brush, &curves)) {
        (*prims)[BrushRingPath()] =
            _Prim{_curvesType,
                  _BuildOverlayDataSource(curves, OverlayMaterialPath())};
        if (brush != _publishedBrush) {
            dirty.brush = usdGenTonic::TonicDirty_Brush;
        }
        dirty.brushCountChanged =
            curves.CurveCount() != _publishedBrushCurves;
        _publishedBrushCurves = curves.CurveCount();
    } else {
        _publishedBrushCurves = 0;
    }
    _publishedGizmo = gizmo;
    _publishedBrush = brush;
    return dirty;
}

void
UsdGenTonicSceneIndex::_CollectLevelDirties(
    usdGenTonic::TonicStagedModel const &previous,
    usdGenTonic::TonicStagedModel const &current, uint32_t modelDirty,
    HdSceneIndexObserver::DirtiedPrimEntries *out) const
{
    for (auto const &kv : current.levels) {
        int const level = kv.first;
        usdGenTonic::TonicStagedLevel const &now = kv.second;
        auto prevIt = previous.levels.find(level);
        if (prevIt == previous.levels.end()) {
            continue;  // brand new level: PrimsAdded covers it
        }
        usdGenTonic::TonicStagedLevel const &was = prevIt->second;
        uint32_t dirty = 0;
        if (was.topologyHash != now.topologyHash) {
            dirty |= usdGenTonic::TonicDirty_Topology;
        } else if (was.pointsHash != now.pointsHash) {
            dirty |= usdGenTonic::TonicDirty_Points;
        }
        if (was.uniformHash != now.uniformHash) {
            dirty |= Dirty_Uniforms;
        }
        if (was.selectionHash != now.selectionHash) {
            dirty |= usdGenTonic::TonicDirty_Selection;
        }
        if (was.focused != now.focused ||
            was.displayScale != now.displayScale) {
            dirty |= Dirty_Widths;
        }
        if (was.xray != now.xray || was.xrayOpacity != now.xrayOpacity) {
            dirty |= Dirty_Xray;
        }
        if (was.xray != now.xray) {
            // Opaque and x-ray are two materials, not two parameters.
            dirty |= Dirty_Material;
        }
        if (was.visible != now.visible ||
            was.centersOnly != now.centersOnly) {
            dirty |= Dirty_Visibility;
        }
        HdDataSourceLocatorSet const mesh = NoticesFor(dirty);
        if (!mesh.IsEmpty()) {
            out->push_back({TubesPath(level), mesh});
        }
        // The overlays never carry x-ray and never swap material;
        // everything else applies. The centers flag is theirs alone: it
        // hides the center curves and CV dots without touching the mesh.
        uint32_t overlay = dirty & ~(Dirty_Xray | Dirty_Material);
        if (was.centers != now.centers) {
            overlay |= Dirty_Visibility;
        }
        // Ring visibility (plan/18 §2.4a) rides on its own hash, so that
        // selecting a tube dirties the two ring prims and nothing else.
        uint32_t const ringOverlay =
            was.ringHash == now.ringHash
                ? overlay
                : (overlay | usdGenTonic::TonicDirty_Topology |
                   Dirty_Visibility);
        HdDataSourceLocatorSet const curves = CurveNoticesFor(overlay);
        if (!curves.IsEmpty()) {
            out->push_back({CentersPath(level), curves});
        }
        HdDataSourceLocatorSet const ringCurves = CurveNoticesFor(ringOverlay);
        if (!ringCurves.IsEmpty()) {
            out->push_back({RingsPath(level), ringCurves});
        }
        HdDataSourceLocatorSet const points = PointNoticesFor(overlay);
        if (!points.IsEmpty()) {
            out->push_back({CenterCVsPath(level), points});
        }
        HdDataSourceLocatorSet const ringPoints = PointNoticesFor(ringOverlay);
        if (!ringPoints.IsEmpty()) {
            out->push_back({RingCVsPath(level), ringPoints});
        }
        if (now.guideCount > 0 && was.guideCount > 0) {
            uint32_t const guideDirty =
                (modelDirty & usdGenTonic::TonicDirty_Guides) |
                (dirty & Dirty_Visibility);
            HdDataSourceLocatorSet const guides = GuideNoticesFor(
                guideDirty, now.guideCount != was.guideCount);
            if (!guides.IsEmpty()) {
                out->push_back({GuidesPath(level), guides});
            }
        }
    }
}

void
UsdGenTonicSceneIndex::_Commit(
    std::map<SdfPath, _Prim> &&prims,
    HdSceneIndexObserver::DirtiedPrimEntries const &dirtied)
{
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto const &kv : _prims) {
            if (!prims.count(kv.first)) {
                removed.push_back({kv.first});
            }
        }
        for (auto const &kv : prims) {
            auto it = _prims.find(kv.first);
            if (it == _prims.end() ||
                it->second.primType != kv.second.primType) {
                added.push_back({kv.first, kv.second.primType});
            }
        }
        _prims = std::move(prims);
    }
    if (!removed.empty()) {
        _SendPrimsRemoved(removed);
    }
    if (!added.empty()) {
        _SendPrimsAdded(added);
    }
    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
}

bool
UsdGenTonicSceneIndex::PublishModel(usdGenTonic::TonicModel *model,
                                    uint32_t dirtyMask)
{
    std::map<SdfPath, _Prim> prims;
    prims[RootPath()] = _Prim{_scopeType, HdRetainedContainerDataSource::New()};
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    usdGenTonic::TonicStagedModel staged;
    uint32_t modelDirty = dirtyMask;
    _OverlayDirty overlayDirty;
    bool anything = false;
    int restaged = 0;

    if (model != _stagedModel) {
        _publisher->Clear();
        _stagedModel = model;
    }
    if (model) {
        // A model has been active: the test tube is a harness convenience,
        // not a fallback, and never shares the frame with real geometry.
        _testTubeRetired = true;
        modelDirty |= model->TakeDirty();
        if (_publisher->Stage(*model)) {
            staged = _publisher->Current();
            restaged = staged.restagedTubeCount;
            _BuildLevelPrims(staged, &prims);
            anything = true;
        }
        _BuildGraphPrims(*model, &prims);
        overlayDirty = _BuildOverlayPrims(*model, &prims);
        anything = anything || model->HasScalp() ||
                   prims.count(GizmoPath()) || prims.count(BrushRingPath());
    } else if (!_testTubeRetired && TfGetEnvSetting(USDGENTONIC_TEST_TUBE)) {
        _BuildTestTubePrims(&prims);
        anything = true;
    }

    if (anything) {
        prims[TubeMaterialPath()] =
            _Prim{_materialType,
                  _SurfaceMaterial(TfToken("UsdGenTonicTube"),
                                   {_tokUnlit, _tokOpacity},
                                   {_Samp(0.0f), _Samp(1.0f)})};
        // The x-ray twin. Same shader, same parameters but one: `opacity`
        // below 1. That single number is the whole mechanism by which an
        // x-rayed tube stops hiding the curves inside it (see the comment
        // on _kXrayMaterialOpacity above).
        prims[TubeXrayMaterialPath()] =
            _Prim{_materialType,
                  _SurfaceMaterial(TfToken("UsdGenTonicTube"),
                                   {_tokUnlit, _tokOpacity},
                                   {_Samp(0.0f),
                                    _Samp(_kXrayMaterialOpacity)})};
        prims[HairMaterialPath()] =
            _Prim{_materialType, _GuidePreviewMaterial()};
        prims[OverlayMaterialPath()] =
            _Prim{_materialType,
                  _SurfaceMaterial(TfToken("UsdGenTonicTube"),
                                   {_tokUnlit}, {_Samp(1.0f)})};
    }

    if (!model) {
        // No active model: the overlays belong to it and go with it.
        _publishedGizmo = usdGenTonic::TonicGizmoRecord();
        _publishedBrush = usdGenTonic::TonicBrushRingRecord();
        _publishedGizmoCurves = 0;
        _publishedBrushCurves = 0;
    }
    // plan/17 §3.2: the cook's tiles follow "show amplified hair" and the
    // gesture bracket. With no model there is nothing suppressing them.
    _SetAmplifiedTilesVisible(model ? staged.amplifiedTilesVisible : true);
    _SetHiddenGuidesPath(model ? CommittedGuidesPath(model->GetGroomPath())
                               : SdfPath());
    _CollectLevelDirties(*_published, staged, modelDirty, &dirtied);
    // The graph overlay restages wholesale; its notices stay as they were.
    HdDataSourceLocatorSet const graph = GraphNoticesFor(modelDirty);
    if (!graph.IsEmpty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (SdfPath const &path : {GraphNodesPath(), GraphEdgesPath(),
                                    GraphRegionsPath()}) {
            if (_prims.count(path) && prims.count(path)) {
                dirtied.push_back({path, graph});
            }
        }
    }

    // The overlays, one prim at a time: moving the brush ring must not
    // dirty a leaf of the gizmo. A record that appeared or vanished is an
    // add/remove, which _Commit sends instead.
    {
        std::lock_guard<std::mutex> lock(_mutex);
        HdDataSourceLocatorSet const gizmoSet = OverlayNoticesFor(
            overlayDirty.gizmo | (modelDirty & usdGenTonic::TonicDirty_Gizmo),
            overlayDirty.gizmoCountChanged);
        if (!gizmoSet.IsEmpty() && _prims.count(GizmoPath()) &&
            prims.count(GizmoPath())) {
            dirtied.push_back({GizmoPath(), gizmoSet});
        }
        HdDataSourceLocatorSet const brushSet = OverlayNoticesFor(
            overlayDirty.brush | (modelDirty & usdGenTonic::TonicDirty_Brush),
            overlayDirty.brushCountChanged);
        if (!brushSet.IsEmpty() && _prims.count(BrushRingPath()) &&
            prims.count(BrushRingPath())) {
            dirtied.push_back({BrushRingPath(), brushSet});
        }
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        *_published = std::move(staged);
        _lastRestagedTubeCount = restaged;
        _testTubePublished = prims.count(TestTubePath()) != 0;
    }
    _Commit(std::move(prims), dirtied);
    return anything;
}

bool
UsdGenTonicSceneIndex::Refresh(uint32_t dirtyMask)
{
    return PublishModel(usdGenTonic::TonicRegistry::Get().Active(), dirtyMask);
}

bool
UsdGenTonicSceneIndex::AmplifiedTilesVisible() const
{
    return _amplifiedTilesVisible.load();
}

size_t
UsdGenTonicSceneIndex::AmplifiedTileCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _tilePaths.size();
}

bool
UsdGenTonicSceneIndex::_SetAmplifiedTilesVisible(bool visible)
{
    if (_amplifiedTilesVisible.load() == visible) {
        return false;
    }
    _amplifiedTilesVisible.store(visible);
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        HdDataSourceLocatorSet const locators{
            HdDataSourceLocator(_tokVisibility)};
        for (SdfPath const &path : _tilePaths) {
            dirtied.push_back({path, locators});
        }
    }
    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
    return true;
}

SdfPath
UsdGenTonicSceneIndex::HiddenGuidesPath() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _hiddenGuidesPath;
}

SdfPath
UsdGenTonicSceneIndex::CommittedGuidesPath(std::string const &groomPath)
{
    if (groomPath.empty() || !SdfPath::IsValidPathString(groomPath)) {
        return SdfPath();
    }
    SdfPath const groom(groomPath);
    if (!groom.IsAbsolutePath() || !groom.IsPrimPath()) {
        return SdfPath();
    }
    return groom.AppendChild(TfToken("Guides"));
}

bool
UsdGenTonicSceneIndex::_SetHiddenGuidesPath(SdfPath const &path)
{
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hiddenGuidesPath == path) {
            return false;
        }
        HdDataSourceLocatorSet const locators{
            HdDataSourceLocator(_tokVisibility)};
        // Both ends: the prim that stops being hidden and the one that
        // starts. Either may be absent from the input, and a dirty on a
        // path no observer knows is harmless.
        if (!_hiddenGuidesPath.IsEmpty()) {
            dirtied.push_back({_hiddenGuidesPath, locators});
        }
        if (!path.IsEmpty()) {
            dirtied.push_back({path, locators});
        }
        _hiddenGuidesPath = path;
    }
    if (!dirtied.empty()) {
        _SendPrimsDirtied(dirtied);
    }
    return true;
}

// -- passthrough --------------------------------------------------------------

void
UsdGenTonicSceneIndex::_PrimsAdded(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::AddedPrimEntries const &entries)
{
    // Input prims pass through; the input never announces tonic paths.
    // The amplified tiles are remembered so a "show amplified hair" flip
    // can dirty exactly their visibility (plan/17 §3.2).
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto const &entry : entries) {
            if (_IsAmplifiedTilePath(entry.primPath)) {
                _tilePaths.insert(entry.primPath);
            }
        }
    }
    _SendPrimsAdded(entries);
}

void
UsdGenTonicSceneIndex::_PrimsRemoved(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::RemovedPrimEntries const &entries)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto const &entry : entries) {
            auto it = _tilePaths.lower_bound(entry.primPath);
            while (it != _tilePaths.end() && it->HasPrefix(entry.primPath)) {
                it = _tilePaths.erase(it);
            }
        }
    }
    _SendPrimsRemoved(entries);
}

void
UsdGenTonicSceneIndex::_PrimsDirtied(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::DirtiedPrimEntries const &entries)
{
    _SendPrimsDirtied(entries);
}

PXR_NAMESPACE_CLOSE_SCOPE
