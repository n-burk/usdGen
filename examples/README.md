# usdGen examples

Working scenes. The CPU-lane examples (a plain `Xform` parent, no
`UsdGenGroom`) display in usdview:

    .\bin\launch_usdview.ps1 examples\clump-ptex-plane.usda

and render headlessly:

    .\bin\record_usd.ps1 -Scene examples\clump-ptex-plane.usda `
        -Output out.png -Camera /World/Cam -Complexity veryhigh

| Scene | Lane | Shows |
|---|---|---|
| `scatter-grow-plane.usda` | CPU | the smallest groom: Scatter -> Grow |
| `styled-fur-plane.usda` | CPU | Grow -> Noise -> Length -> Width |
| `expression-width-plane.usda` | CPU | connected SeExpr parameters: a root-to-tip width and a frizz mask |
| `guide-interpolate-plane.usda` | CPU | a guide-curve groom: strands interpolated between sparse guides, parted by a region field. The `region` variant set picks a `geoSampler` voronoi over the guide roots (`voronoi`), a Ptex region map (`ptexMap`) or no regions (`smooth`) |
| `clump-ptex-plane.usda` | CPU | clumping from Ptex maps: a region map decides clump membership, a second map the clump tightness; two clump levels |
| `rbf-guides-plane.usda` | CPU | hair deformed by animated curves: `UsdGenDeform` with `usdGen:guides` bends the groom through a cubic RBF over 16 driver curves (frames 1-48; press play) |
| [motion/](motion/README.md) | CPU | seven 100-frame demos: squash/stretch sphere, groom outside the moving Xform, animated braids, combined guide/surface deformation, simulated centers, and Ptex-separated two-center growth/deformation; rest growth is reused during playback |
| `cuda-width-network.usda` | CUDA | CurveSource -> Width on the device lane (cooks; stock Storm cannot display device-resident curves yet) |
| `cuda-rbf-network.usda` | CUDA | CurveSource -> RBF Deform -> Width |
| `cuda-length-network.usda` | CUDA | CurveSource -> Length -> Width |

`tests/testUsdGenGroomExamples`, `testUsdGenRbfDeform`,
`testUsdGenExpressionWidthExample` and `testUsdGenCudaHierarchy` cook these
files. An animated scene renders one frame at a time; `usdrecord` wants a
`###` frame placeholder in the output name:

    .\bin\record_usd.ps1 -Scene examples\rbf-guides-plane.usda `
        -Output out.###.png -Camera /World/Cam -Complexity veryhigh -Frame 20

## Sampling external data in expressions

An expression reads data outside its own strand only through a relationship
on its `UsdGenExpression` prim named `input:<name>`:

    def UsdGenExpression "voronoiRegion"
    {
        string usdGen:expr:source = """geoSampler("guideCurves", "$index")"""
        rel input:guideCurves = </World/Guides>
        custom float outputs:result
    }

* `geoSampler(input, expression [, iterate [, reduce [, query]]])` iterates the
  `point`, `prim` (default) or `geometry` elements of the meshes, curves and
  points the relationship targets (a group prim contributes the gprims under
  it), evaluates the one-line element expression, and reduces with
  `nearest` (default), `nearest2`, `min`, `max`, `sum` or `mean` relative to
  the query position (default: the calling element's `$P`). The element
  expression reads `$P $Pref $N $rootP $rootPref $index $count $id
  $primIndex $primCount $pointIndex $pointCount $t $cLength` of the element,
  plus `$Q` (the query) and `$Qdist` (the element's distance to it).
* `ptex(input)` reads the `UsdGenPtexMap` the relationship targets at the
  strand root; `usdGen:map:channel`, `scale`, `offset`, `clamp` and
  `default` apply.

Because the data is named by a relationship, it is a tracked dependency: an
edit of the targeted prims, or a retarget of the relationship, recooks the
consuming operators. Both functions run on the CPU lane only.

## Seeing a value on the hair

A description can carry a flat value preview in the session layer. Author
`usdGen:preview:source`, `colorMap`, `range`, `evaluation` and `shading`
on the description:

    over "Fur"
    {
        rel usdGen:preview:source = </World/Groom/Fur/Maps/clumpRegions>
        uniform token usdGen:preview:colorMap = "ids"
        uniform token usdGen:preview:shading = "flat"
    }

## Strand-hair scenes

`head-hair-closeup.usda` is a procedural head groom, generator
`tools/make_head_hair.py`. `TempleCam` is the dense-coat view, `HeadCam` the
wider one. Shading notes are in plan/16 and `docs/storm-fur.md`.

Each has a `*-render.usda` companion that adds a backdrop sphere for offline
renders only — a 260-unit backdrop in the viewable scene blows out usdview's
bbox-derived near/far and clips the hair on zoom.

Look colour is authored as `usdGen:look:rootColor` and `tipColor` on the
description. See "Binding your own material loses half the look" in
`docs/storm-fur.md` before binding a different material.

    .\bin\render_hair_parity.ps1 -Label <name>              # renders\hair-parity\<name>_*
    .\bin\render_hair_parity.ps1 -Label ref -Supersample 4  # converged reference, offline only

## Regenerating

    python examples\tools\make_examples.py       # the procedural scenes
    python examples\tools\make_head_hair.py      # head-hair-closeup{,-render}.usda
    .\examples\tools\render_examples.ps1          # renders\examples\*.png
