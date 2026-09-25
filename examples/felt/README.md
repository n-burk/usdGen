# Felt sphere

Open `felt_sphere.usda` with the usdGen plugins loaded. Use camera `/Camera`
and Storm or **Moonray (debug)**, with scene materials and scene lights enabled.

The 24 cm diameter sphere is a welded all-quad Catmull–Clark mesh. Three live
usdGen descriptions copy the current Puppet A yellow felt: dense nap, raised
curl clumps, and fine strays. Scattering evaluates the level-3 limit surface.
Ptex controls and surface textures are baked for this sphere's face topology;
all runtime texture dependencies are relative files in `textures/`.

The example uses 1.65 million nap roots/m², 450,000 curl roots/m² and 7,000
strays/m², with the same physical fibre lengths, widths and clump settings as
the puppet. It has no facial holdouts and no authored PointInstancer or
BasisCurves replacement groom.

## Animated sphere (100 frames)

Open `felt_sphere_animated.usda`, select `/World/Camera`, and play frames 1–100
at 24 fps. `/World/Motion` moves left/right; its child sphere has point-sampled,
volume-preserving squash/stretch. The groom is a sibling under that Xform.

The execution order is:

`Scatter → Grow → Curl → Width → surfaceAnimate (UsdGenDeform)`

Scatter uses the sphere's **Default-time** rest mesh and Catmull–Clark limit
surface at isolation level 3. Grow/style are static. The final surface-driven
RBF uses 64 mesh vertex samples, `lockRoots=false`, and deforms every strand CV.
The shared parent transform is applied once by Hydra, after local deformation.
There are 3,188 strands / 25,504 CVs, generated live with no authored curves.

This example exercises the CPU surface-RBF extension in `UsdGenDeform` and the
rest-only Scatter cache key. Rebuild usdGen and restart an existing viewer to
load those changes. The CPU surface and groom must share object space; no
guide-curve adapter is needed. RBF exactly reproduces this example's affine
squash/stretch, which also preserves attachment to the deformed limit surface.
For arbitrary nonlinear deformation, RBF is an approximation, not a per-CV
limit-surface wrap operator.

`make_animated_sphere.py` regenerates the scene using the usdGen/OpenUSD Python
environment. `check_animated_sphere.py` is a testusdview script that captures all
100 frames. `animation_validation.json` records the playback/cache checks.
