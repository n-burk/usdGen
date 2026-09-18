param(
    [string] $OutDir = ""
)

# Renders the procedural examples (and every region variant of the guide
# example) with bin\record_usd.ps1, for a quick visual check.
#
#   .\examples\tools\render_examples.ps1 -OutDir $env:TEMP\usdGenExamples

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $OutDir) { $OutDir = Join-Path $Root "renders\examples" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$record = Join-Path $Root "bin\record_usd.ps1"
$examples = Join-Path $Root "examples"

function Render([string] $scene, [string] $name) {
    & $record -Scene $scene -Output (Join-Path $OutDir "$name.png") -Camera /World/Cam -Complexity veryhigh
}

# A variant is selected by a throwaway layer that sublayers the example; the
# example's relative map paths still resolve against its own layer.
foreach ($variant in @("voronoi", "ptexMap", "smooth")) {
    $wrapper = Join-Path $OutDir "guide-interpolate-$variant.usda"
    $source = (Join-Path $examples "guide-interpolate-plane.usda").Replace('\', '/')
    @"
#usda 1.0
(
    defaultPrim = "World"
    subLayers = [@$source@]
)

over "World"
{
    over "Groom"
    {
        over "Hair" (
            variants = {
                string region = "$variant"
            }
        )
        {
        }
    }
}
"@ | Set-Content -Encoding ascii $wrapper
    Render $wrapper "guide-interpolate-$variant"
}
Render (Join-Path $examples "clump-ptex-plane.usda") "clump-ptex-plane"
Render (Join-Path $examples "scatter-grow-plane.usda") "scatter-grow-plane"
Render (Join-Path $examples "styled-fur-plane.usda") "styled-fur-plane"
Render (Join-Path $examples "expression-width-plane.usda") "expression-width-plane"
# An animated scene: usdrecord numbers the frame into the ### placeholder.
& $record -Scene (Join-Path $examples "rbf-guides-plane.usda") `
    -Output (Join-Path $OutDir "rbf-guides-plane.###.png") -Camera /World/Cam `
    -Complexity veryhigh -Frame 20
