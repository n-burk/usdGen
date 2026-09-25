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

The 100-frame animation demos now live in [examples/motion](../motion/README.md).
