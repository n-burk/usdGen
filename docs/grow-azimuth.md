# Grow azimuth and isotropic felt

`UsdGenGrow` applies direction controls in this order:

1. Normalize the surface normal or authored `directionVector`.
2. Rotate by `usdGen:lift` about the root binormal B.
3. Rotate by azimuth about the root normal N.

The two native azimuth controls are:

| Attribute | Range | Default | Meaning |
| --- | --- | --- | --- |
| `float usdGen:azimuth` | -360 to 360 degrees | 0 | Fixed rotation around N after lift |
| `float usdGen:azimuthRandom` | 0 to 1 | 0 | Fraction of a full-circle random azimuth distribution |

The final angle is `azimuth + azimuthRandom * 360 * (draw - 0.5)`.
The draw is uniform in `[0,1)` and keyed by the operator seed, stable strand
ID and dedicated `kSaltGrowAzimuth` salt (`0x4772417A`). It is independent of
length variation, evaluation order and chunk boundaries. Both controls at
zero preserve the existing Grow geometry exactly. A zero lift on the normal
has no lean to turn; azimuth alone will not tilt it.

## Felt example

```usda
def UsdGenGrow "grow"
{
    token usdGen:direction = "surfaceNormal"
    float usdGen:lift = 42
    float usdGen:azimuth = 0
    float usdGen:azimuthRandom = 1
    int usdGen:seed = 21
}
```

For isotropic felt, full random azimuth prevents all fibres on a face from
leaning toward that face's local U direction. The limit surface can be
smooth while its parameter charts rotate between adjacent faces; a fixed
face-local lean exposes those chart boundaries through fibre shading.
Randomization removes this systematic directional bias, without modifying
surface normals, root locations, Ptex face IDs/UVs, density or length.
It does not repair a faceted emitter or provide a continuous comb field for
deliberately directional fur.

Both controls support groom/primitive expressions on the CPU, including
Ptex expressions. Point-domain connections are rejected. The CUDA
Scatter/Grow and CurveSource/Grow producers support literal controls and
use the same angle/hash convention. Connected Grow controls retain the
existing CUDA admission policy: unsupported graphs fail admission rather
than silently ignoring the connection. Limit-surface Scatter remains CPU.

The controls are value-class properties, included in Grow capture identity;
capture stores per-strand angles for matching rest and evaluated geometry.
NaN, infinity and values outside the documented ranges are rejected.
Schema property metadata exposes the controls to generic property editors.

Regression coverage lives in `testUsdGenCpuFanout`,
`testUsdGenExpressionBindings`, `testUsdGenCudaScatterGrow`,
`testUsdGenCudaCurveGrow`, `testUsdGenCudaScatterGrowSession` and
`testUsdGenCudaCurveGrowSession`.
The CPU regression includes four quad charts whose tangent axes differ by
90 degrees: every chart must retain a full-circle distribution, and changing
face IDs while retaining strand IDs must leave all Grow points unchanged.
