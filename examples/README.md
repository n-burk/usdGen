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
| `cuda-width-network.usda` | CUDA | CurveSource -> Width on the device lane (cooks; stock Storm cannot display device-resident curves yet) |
| `cuda-rbf-network.usda` | CUDA | CurveSource -> RBF Deform -> Width |
| `cuda-length-network.usda` | CUDA | CurveSource -> Length -> Width |

`tests/testUsdGenGroomExamples`, `testUsdGenRbfDeform`,
`testUsdGenExpressionWidthExample` and `testUsdGenCudaHierarchy` cook these
files. An animated scene renders one frame at a time; `usdrecord` wants a
`###` frame placeholder in the output name:

    .\bin\record_usd.ps1 -Scene examples\rbf-guides-plane.usda `
        -Output out.###.png -Camera /World/Cam -Complexity veryhigh -Frame 20

## Profiling playback

`bin/trace_playback.ps1` plays a scene and writes an OpenUSD trace
(`build/traces/<scene>/` unless `-OutDir` says otherwise):

    .\bin\trace_playback.ps1 -Scene examples\rbf-guides-plane.usda -Pull
    .\bin\trace_playback.ps1 -Scene examples\rbf-guides-plane.usda -Usdview -TraceFormat trace

The first runs `usdGenTracePlayback`, which steps the frames through the groom
scene index the way usdview does, and writes these files:

* `playback.log`: a table of the time spent in each frame, and a hash of the
  published points to compare two builds with. For each cook it also says:
  * why it compiled;
  * which operators re-captured, and why;
  * how many chunks each evaluated;
  * where the cook's time went;
  * which dirty locators reached Hydra.
* `trace.json`: for `chrome://tracing` or https://ui.perfetto.dev.
* `report.txt`: OpenUSD's aggregate tree.

`-Quiet` keeps only the table. `-Usdview` plays the scene in testusdview
instead and adds Storm's sync and draw to the trace. Its table splits each
frame into `setFrame`, `paintGL` and the rest of Qt's work (widget updates,
compositing, and a buffer swap that waits for vsync). A Chrome trace of Storm
is hundreds of MB; `-TraceFormat trace` writes the aggregate tree instead.
The per-cook lines are `TF_DEBUG` codes, so any host can print them:

    $env:TF_DEBUG = "USDGEN_COMMIT USDGEN_SCHEDULE USDGEN_INGRESS"

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

The SeExpr editor's **Colour hair by value** group (usdview, `Ctrl+Shift+E`)
replaces the hair material with a flat preview and colours every strand by:

* the edited expression, evaluated per strand, per CV or once;
* a Ptex map the expression reads (`Distinct ids` shows one colour per clump
  or region cell);
* the attribute the expression drives, or any operator attribute through
  **Show on hair** in the Connections tab: the values the operator was cooked
  with, or its authored value when nothing drives it (`usdGen:mask`, say).

With **Follow edits** on, every edit that compiles is previewed as you type.
The editor writes `usdGen:preview:source`, `colorMap`, `range`, `evaluation`
and `shading` on the description in the session layer only; the same
properties can be authored by hand:

    over "Fur"
    {
        rel usdGen:preview:source = </World/Groom/Fur/Maps/clumpRegions>
        uniform token usdGen:preview:colorMap = "ids"
        uniform token usdGen:preview:shading = "flat"
    }

## UE-parity hair scenes

Two scenes exist to compare `UsdGenHairStrands` against Unreal's strand
shading (plan/16, `docs/storm-fur.md`):

* `head-hair-closeup.usda` — a procedural head groom, generator
  `tools/make_head_hair.py`. `TempleCam` is the dense-coat view, `HeadCam` the
  wider one.
* `metahuman-hair-parity.usda` — the real converted MetaHuman groom under
  `plan/examples/metahuman-hair/male_hair_01/`, routed through a
  `UsdGenCurveSource` so it picks up the density bake, the scalp shadow and the
  default strand material. Nothing under `plan/examples/` is modified; the
  scene sublayers it and overrides in place. Generator
  `tools/make_metahuman_parity.py`.

Each has a `*-render.usda` companion that adds a backdrop sphere for offline
renders only — a 260-unit backdrop in the viewable scene blows out usdview's
bbox-derived near/far and clips the hair on zoom.

The colour of the MetaHuman groom is authored as `usdGen:look:rootColor` and
`tipColor` on the description, with **no material bound**, because only the
publisher's synthetic default material carries the look's tip colour. See
"Binding your own material loses half the look" in `docs/storm-fur.md` before
adding one.

    .\bin\render_ue_parity.ps1 -Label <name>              # renders\ue-parity\<name>_*
    .\bin\render_ue_parity.ps1 -Label ref -Supersample 4  # converged reference, offline only

## Regenerating

    python examples\tools\make_examples.py       # the two procedural scenes
    python examples\tools\make_head_hair.py      # head-hair-closeup{,-render}.usda
    python examples\tools\make_metahuman_parity.py   # metahuman-hair-parity{,-render}.usda
    .\examples\tools\bake_maps.ps1                # examples\maps\*.ptx (usdGenBakePtex)
    .\examples\tools\render_examples.ps1          # renders\examples\*.png
