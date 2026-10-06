<!-- Copyright (c) 2026 Nick Burkard -->
<!-- SPDX-License-Identifier: MIT -->

# ALab stoat for groom examples

The character in these examples comes from [Netflix Animation Studios
ALab](https://github.com/DigitalProductionExampleLibrary/ALab), originally
created by Animal Logic and now distributed through the [ASWF Digital
Production Example Library](https://dpel.aswf.io/alab/). The prepared asset is
local output, not part of usdGen's MIT-licensed source tree.

From the repository root, with OpenUSD 26.08 installed:

```sh
python tools/prepare_alab.py prepare --usdcat "$USD/bin/usdcat"
```

This reads the official ALab v2.3.0 structure files and extracts only the
stoat render geometry, materials, textures, and skeleton from the v2.2.0
Techvar archive using HTTP byte ranges. It produces:

| Local path | Contents |
|---|---|
| `out/alab/stoat01.usdc` | Single flattened USD stage: body, outfit, backpack, preview and full materials, skin weights, and skeleton |
| `out/alab/textures/` | 213 relative 1K texture sidecars used by the USD |
| `out/alab/stoat01.usdz` | Portable package with USD, textures, preview cards, and upstream license |
| `out/alab/provenance.json` | Source versions, paths, and modification record |
| `out/alab/ALab-LICENSE.md` | Upstream ASWF Digital Assets License v1.1 |

The character root is `/Stoat`; the groomable skin mesh is
`/Stoat/body_M_hrc/GEO/body_M_geo`. The source has an authored 44-joint
UsdSkel skeleton at `/Stoat/RIG/root_jnt` and mesh skin bindings. Its included
`SkelAnimation` authors only the root joint. The prepared static character
does not claim the shot's full motion cache. The skeleton variant is enabled
and the source `render_high` geometry is selected. ALab's procedural fur is
intentionally left off so usdGen can generate the groom.

The source outfit's `full_ao_texture` points to a UDIM name absent from the
Techvar archive. Preparation sets that one material input to neutral
occlusion 1. Flattening also removes ALab-specific Maya metadata fields that
OpenUSD 26.08 does not recognize; it preserves renderable geometry, materials,
and skinning data. Texture paths are rewritten to relative sidecars and all
authored paths are checked before the package is emitted.

OpenUSD 26.08 `usdcat` loads the consolidated USDC and USDZ, and explicit
sidecar and package-reference checks pass. `usdchecker` still exits nonzero
on this asset with UDIM-template resolution and normal-map validation
errors. Treat that checker result as an open validation limit rather than a
clean pass.

The ALab assets use the **ASWF Digital Assets License v1.1**, not usdGen's MIT
license. Redistribution of these derived assets for demonstrations is subject
to the upstream license and requires its copyright, conditions, disclaimer,
and a description of modifications. Publications showing images derived from
ALab must include this notice:

> Netflix Animation Studios ALab Copyright 2025 Netflix, Inc. All rights reserved.

## Rebuild the usdGen groom

With the consolidated asset prepared, run:

```sh
python -S tools/make_stoat_groom.py
```

This writes `stoat-groom.usda`, `stoat-bare.usda`, and `stoat-guides.usda` in
this directory. The first references `out/alab/stoat01.usdc` and does not copy
ALab data into the repository. The body coat is divided into brown back,
cream belly, brown head, and short cream face regions by face subsets on the
actual ALab skin. Mouth faces are omitted and the separately modeled eyes,
nose, paws, and tongue stay visible. Each region has sparse authored guides
that drive usdGen Scatter → GuideInterpolate → Noise → Width. The guide layer
exposes those controls over the dense coat; the bare layer turns the groom off
for comparison.

ALab's original tail tuft consists of long rigid spikes. In the groom study,
that mesh is hidden and a smooth scalp volume fitted to its bounds supplies
short, swept tail fibres with a darker terminal region. The original tuft is
unchanged in `out/alab/stoat01.usdc` and appears in the bare comparison.
The outfit and backpack remain in the consolidated asset but are hidden in
the groom views to expose the body. The groom shows a static pose; its guides
have not been skinned to the ALab skeleton.

For a local Storm render with the built usdGen plugin:

```sh
python -S tools/_usdrecord_clean.py --renderer GL --imageWidth 1536 \
  --disableCameraLight --camera /World/HeroCam --complexity veryhigh \
  examples/alab/stoat-groom.usda out/alab/stoat-groom.png
```

`/World/DetailCam` frames the face and `/World/ProfileCam` shows the coat
boundary in profile. The scene is reproducible from the downloaded ALab
asset and the checked-in generator; rendered images in the manual are output
from these USD scenes, not painted illustrations.

For the final studio stills, the standalone `tools/docs` target
`usdGenBakeGroom` cooked the six descriptions in order—BrownBody, CreamBody,
BrownHead, CreamFace, Tail, DarkTip—into native USD BasisCurves. The final
render contained **188,914 curves**, with cooked points, CV widths and color.
Each pass has the form `usdGenBakeGroom <previous-stage> /World/Groom/<region>
<next-stage.usdc>`, beginning with `stoat-groom.usda`. The resulting bake is
local render scratch and may contain absolute texture references after USD
export; use `out/alab/stoat01.usdc` for the portable consolidated ALab asset
and `stoat-groom.usda` for the editable procedural recipe.

Use ALab's name only to identify the asset. This example does not imply
endorsement by Netflix Animation Studios or its contributors. See the
[upstream license](https://github.com/DigitalProductionExampleLibrary/ALab/blob/main/LICENSE.md)
and the copy packaged with the prepared asset for the complete terms.
