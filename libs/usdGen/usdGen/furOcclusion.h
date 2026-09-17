#ifndef USDGEN_FUR_OCCLUSION_H
#define USDGEN_FUR_OCCLUSION_H

#include "usdGen/curveBuffer.h"

namespace usdGen {
class UsdGenWorkDispatcher;

/// Opaque geometry injected into the fur density volume, Unreal's
/// `r.HairStrands.Voxelization.InjectOpaqueDepth`. Without it light leaks
/// through the scalp and hair on the unlit side of a head stays lit. `points`
/// are in the mesh's own object space; `worldMatrix` places them in the same
/// world space as the tiles' transformed points. Polygons are fan-triangulated
/// and their shell is marked `opaqueBiasVoxels` INSIDE the surface so roots
/// sitting on it are not self-shadowed.
struct UsdGenFurOccluder
{
    VtVec3fArray points;
    VtIntArray   faceVertexCounts;
    VtIntArray   faceVertexIndices;
    GfMatrix4d   worldMatrix{1.0};
};

struct UsdGenFurOcclusionParams
{
    /// Opaque blockers, normally the groom's emitting surfaces. Only the part
    /// that falls inside the groom's own bounds is voxelized: the grid is
    /// never grown to fit an occluder, so a ground plane cannot destroy the
    /// resolution of a head.
    std::vector<UsdGenFurOccluder> occluders;

    /// Target voxel edge in world units when `resolution` is 0. Unreal uses
    /// 0.3 cm (`Voxelization.Virtual.VoxelWorldSize`) and this scene
    /// convention is centimetres. The bake never chooses a voxel coarser than
    /// the historical 48-cube fitted to the longest axis, and never a grid
    /// dimension above `maxDimension`.
    float voxelSize = 0.3f;
    int   maxDimension = 128;

    /// > 0 overrides the derived grid with a cube of this many voxels fitted
    /// to the longest axis (the pre-2026-09 behaviour and the bake CLI's
    /// explicit argument). Clamped to [8, maxDimension].
    int   resolution = 0;

    /// Unreal's `InjectOpaque.BiasCount` / `MarkCount`, in voxels: the opaque
    /// shell starts this far below the surface and is this thick.
    int   opaqueBiasVoxels = 2;
    int   opaqueMarkVoxels = 4;

    /// Also build the scalp-shadow cap (UsdGenScalpShadowPublication): the
    /// occluders' haired area, tessellated to about the voxel size, carrying
    /// HAIR-ONLY optical depth so it darkens the skin by what the hair above
    /// absorbs and not by the head shadowing itself, which the skin shader's
    /// own N.L already does.
    bool scalpShadow = false;
    /// Ceiling on the cap's triangle count; the tessellation level is the
    /// largest that fits it and still does not exceed the voxel size.
    int  scalpMaxTriangles = 200000;

    /// With a dispatcher, tiles are splatted and gathered in parallel on its
    /// arena and the occluder shell is marked in parallel slabs.
    UsdGenWorkDispatcher* dispatcher = nullptr;
};

/// Bakes geometry-derived directional optical depth for a whole groom.
///
/// SEMANTICS (matched to Unreal's `FHairTransmittanceMask::HairCount`, which
/// is what `ComputeDualScatteringTerms` consumes):
///   furTauP[k] / furTauN[k] = the EXPECTED NUMBER OF FIBRE CROSSINGS for a
///   ray leaving the CV toward +axis[k] / -axis[k] and running to the edge of
///   the volume -- the line integral of projected fibre area per unit volume.
/// A single fibre of width w crossing the ray's voxel column contributes w/h,
/// not 1, exactly as a voxel density volume does: these are expectations over
/// the voxel's cross-section, so a dense groom reads tens of crossings and an
/// isolated strand reads a few hundredths.
///
/// The receiver's OWN contribution IS included (half of its own voxel, the
/// correct discretisation of an integral that starts at the CV). That matches
/// Unreal, whose ray march also starts inside the shaded strand's voxel, so
/// the consumer must apply Unreal's shift, `HairCount = max(0, tau - 1)`,
/// before `Tf = pow(A_front, HairCount)`.
///
/// RECONSTRUCTION. For a world-space direction L the consumer must use the
/// normalised cosine-power-4 blend of the six stored depths:
///   vec3 d = mix(furTauN, furTauP, step(vec3(0), L));
///   vec3 w = L*L; w *= w;                       // cos^4, per axis
///   float tau = dot(w, d) / (w.x + w.y + w.z);  // >= 1/9 for a unit L
/// Power 4 beats the older power-2 blend (`dot(L*L, d)`, which needed no
/// normalisation) by ~17% RMS on transmittance for the same data; see
/// docs/storm-fur.md for the measured table. Values saturate at 64.
///
/// Opaque occluders are folded into the same hair count as a saturated shell,
/// so the consumer needs no separate visibility channel: one crossing of the
/// shell already reaches the clamp.
///
/// All tiles of a description participate in one volume, so tile boundaries do
/// not become lighting boundaries. There is no light or camera dependency.
///
/// Returns false when nothing changed and the previous COW planes were shared.
/// `previous` alone cannot see a parameter or occluder edit, so pass
/// `volumeKey`: caller-owned state, 0 on the first call, written back every
/// call. Without it, occluder/resolution edits are silently reused.
///
/// `scalpShadow` receives the cap when `params.scalpShadow` is set AND the bake
/// rebuilds. On the reuse path it is left untouched, so a caller that wants the
/// COW share seeds it from the previous generation before calling. Its depths
/// are HAIR ONLY and carry no self shift: the receiver is skin, not a fibre, so
/// the consumer uses the count as it stands rather than Unreal's
/// `max(0, HairCount - 1)`.
///
/// Throws std::invalid_argument for malformed or non-finite geometry.
bool UsdGenBuildFurOcclusion(
    std::vector<UsdGenTilePublication>* tiles,
    std::vector<UsdGenTilePublication> const* previous = nullptr,
    UsdGenFurOcclusionParams const& params = UsdGenFurOcclusionParams(),
    uint64_t* volumeKey = nullptr,
    UsdGenScalpShadowPublication* scalpShadow = nullptr);

/// The saturation ceiling of the published depths.
inline constexpr float UsdGenFurTauClamp = 64.0f;
}
#endif
