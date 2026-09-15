# MetaHuman hair examples

Three static hair previews share one skin mesh. Each hairstyle stores its imported
strands and guides in separate USD crate files. Open a hairstyle's `hair.usda`
to compose the skin, strands, guides, preview materials, lights, and camera.

## Files

```text
metahuman-hair/
├── README.md
├── skin.usdc
├── female_hair_01/
│   ├── hair.usda
│   ├── guides.usdc
│   └── strands.usdc
├── male_hair_01/
│   ├── hair.usda
│   ├── guides.usdc
│   └── strands.usdc
└── male_hair_02/
    ├── hair.usda
    ├── guides.usdc
    └── strands.usdc
```

Each `hair.usda` uses relative sublayers: `guides.usdc`, `strands.usdc`, and
`../skin.usdc`. Keep this directory structure intact when moving the examples.
All geometry needed for these previews is included; no Alembic plugin or
external texture downloads are required.

| Example | Imported source | Strands | Guides |
| --- | --- | ---: | ---: |
| `female_hair_01` | `groom3.usdc` | 20,036 | 2,092 |
| `male_hair_01` | `groom4.usdc` | 20,036 | 2,092 |
| `male_hair_02` | `groom5.usdc` | 20,036 | 2,092 |

The source names identify the local converted groom layers used to prepare these
examples; those original combined files are not required or included. Splitting
preserves the retained prim names, transforms, attributes, and relationships.

## Open in usdview

With a Python-enabled OpenUSD installation and `usdview` on PATH, run from the
repository root:

```sh
usdview plan/examples/metahuman-hair/female_hair_01/hair.usda
usdview plan/examples/metahuman-hair/male_hair_01/hair.usda
usdview plan/examples/metahuman-hair/male_hair_02/hair.usda
```

These are standard `Mesh` and `BasisCurves` previews: viewing the imported hair
does not require building usdGen. The custom usdGen attributes remain available
for inspection; registering the usdGen schemas enables their schema definitions.

To use the repository's plugin-enabled Windows launcher, set `USD` to your
OpenUSD installation prefix and `PY` to its compatible Python interpreter, then
build and launch from the repository root:

```powershell
$env:USD = 'C:\path\to\OpenUSD-install'
$env:PY = 'C:\path\to\python.exe'
.\bin\build_usdgen.ps1 -UsdInstallDir $env:USD -NoCuda
.\bin\launch_usdview.ps1 plan\examples\metahuman-hair\female_hair_01\hair.usda
```

Substitute `male_hair_01` or `male_hair_02` to view the other styles. In usdview,
select `/World` and press **F** to frame the scene. Guides have `purpose = guide`;
enable guide-purpose display in the viewport to inspect them.
They are excluded from the default-purpose hair preview.

## Scene conventions

- `/World/Scalp`: shared skin `Mesh` from `skin.usdc`.
- `/World/Hair`: imported strand `BasisCurves` from `strands.usdc`.
- `/World/Guides`: imported guide `BasisCurves` from `guides.usdc`.
- `/World/Looks`, `/World/Lights`, `/World/Cam`: preview setup in `hair.usda`.
- `defaultPrim = World`, `upAxis = Z`, `metersPerUnit = 0.01`.
- The `/World` transform converts the preserved local meter/Y-up geometry to
  centimeter/Z-up scene coordinates. Sublayer composition applies that common
  transform once, rather than stacking it for each asset.

The hair and guides retain the imported `UsdGenCurveAPI` metadata and primvars.
The preview materials use `UsdPreviewSurface` with constant colors.

## Execution scope

These files contain imported curves, not a `UsdGenDescription` operator graph.
There is no Scatter/Grow execution or guide-driven interpolation in these
previews. The guides are stored separately for inspection and future grooming
work; simply loading them does not generate or reshape the strands.

For an executable CPU operator example, see
[`../styled-fur-plane.usda`](../styled-fur-plane.usda). Its
Scatter → Grow → Noise → Length → Width chain generates new strands from a
surface. Guide interpolation is declared in the current usdGen schema but has
no registered execution kernel in this checkout.
