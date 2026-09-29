# Capybara Alembic conversion

Open hair.usda. Keep guides.usdc, strands.usdc and skin.usdc beside it.

- strands.usdc: /World/Hair, 709,813 static strands (out_groom.abc).
- guides.usdc: /World/Guides, 2,500 curves. Default points and primvars:rest come from out_guides.abc. Points/widths at frames 1-48 come from groom_animated.abc. UsdGenCurveAPI and guide role are authored; purpose=guide keeps drivers out of ordinary renders.
- skin.usdc: /World/Scalp. Default points and primvars:rest come from out_skin.abc. Points and UV animation at frames 1-48 come from skin_animated.abc. UsdGenRestAPI is authored.

Rest/default time is distinct from frame 1. This matches the rest-plus-animated attribute pattern in <local-examples>/examples/rbf-guides-plane.usda. The source animated guide archive labels its curves as strands, but its curve counts and IDs match out_guides.abc; this conversion identifies them as guides.

The collector uses 24 fps, frames 1-48, centimeter units and Z-up, matching adjacent asset collectors. These particular Alembics contain meter-scale positions. Groom axes are swapped into Y-up local space without dividing by 100. Skin positions are already Y-up local. One shared World transform scales by 100 and rotates +90 degrees around X into centimeter/Z-up stage space. Matching transform opinions in sublayers compose once.

Curves use cubic bspline/pinned, with original CVs and widths; this smooths the original linear Alembic curves. Source groom IDs are preserved, and stable uint64 IDs, hairId, hairT, rest and preview colors are added. No curves needed extra CVs. The sources have no root UVs, so no artificial st or skin binding is generated for the curves. The skin's UVs are retained.

This collector composes geometry and preview materials/lights/camera. It does not add the reference example's procedural UsdGenDeform/RBF operator graph: the dense hair remains static while the skin and driver guides animate. The combined files are ready to be used as its rest/animated inputs.

Validation: every animated point sample and guide width sample was compared against its transformed Alembic source; rest positions, guide IDs and topology correspondence were checked; the collector was reopened successfully. Interactive rendering and RBF deformation were not tested. See conversion_report.json for counts and file sizes.

Rebuild script: <scratch>/convert_cabybara.py (a DCC 22 hython). It refuses existing output files. Source Alembics are unchanged.

## RBF hookup

hair.usda now evaluates CurveSource -> Deform under /World/Groom/Hair. The original collector is hair_static.usda.

The current project schema permits only CUDA execution, and CUDA explicitly rejects usdGen:guides. rbf_inputs.usdc therefore exposes the same guide CVs (Default-time rest plus all 48 point samples) as a hidden vertex-only mesh /World/RBFDrivers. Surface RBF samples those guide CVs, not the rendered skin. The source strands and adapter points are baked into matching centimeter/Z-up world coordinates with identity world transforms, as required by PrepareCudaSurface. The source strands are hidden to avoid double rendering; their rest and IDs remain readable by CurveSource. The original guides and skin files remain unchanged.

Deform uses 100 RBF samples and lockRoots=false so the animal's motion carries the strand roots. Adjust /World/Groom/Hair/Ops/deform.usdGen:rbfSamples to tune accuracy/cost. This is an adapter for the CUDA surface path, not the reference's unavailable CPU guide-input path.

Validation: USD graph targets, default/rest inputs, 48 driver samples and identity world transforms were checked. The project's trace_playback.ps1 process exited with code 1 without diagnostics using the configured <openusd-install> runtime, so live engine/rendered deformation could not be verified here. Reload hair.usda using the built usdGen plugin to evaluate it.

### Playback diagnosis

The configured usdGen build has USDGEN_ENABLE_CUDA=OFF. With its correct Python DLL runtime on PATH, trace_playback reports: "CUDA execution backend factory is unavailable: CUDA support was not enabled in this build" and publishes zero tiles. The collector now explicitly restores /World/Hair visibility so this failure no longer removes the groom. Live RBF needs a compatible CUDA-enabled build; alternatively the deformation can be baked into point time samples. The graph is not a verified working live setup in the current build.
