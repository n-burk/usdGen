param(
    [string] $Build = ""
)

# Bakes the Ptex maps the examples read (examples/maps/*.ptx) with
# usdGenBakePtex, from the skins examples/tools/make_examples.py writes.
#
#   .\examples\tools\bake_maps.ps1
#
# guide_regions.ptx     5 voronoi regions (value = region index) for the
#                       ptexMap variant of guide-interpolate-plane.usda
# clump_regions.ptx     ~90 voronoi cells with a random value each and wobbly
#                       borders: the clump map of clump-ptex-plane.usda
# clump_tightness.ptx   6 large patches of random value: its clump amount map

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Build) { $Build = Join-Path $Root "build" }
$tool = Join-Path $Build "usdGenBakePtex.exe"
if (-not (Test-Path $tool)) { throw "usdGenBakePtex is not built. Run .\bin\build_usdgen.ps1 first." }

$UsdInstallDir = $env:USD
if (-not $UsdInstallDir) { $UsdInstallDir = Join-Path (Split-Path $Root) "usdRig\usd-install" }
$env:PATH = @((Join-Path $UsdInstallDir "lib"), (Join-Path $UsdInstallDir "bin"), $Build, $env:PATH) -join ';'
$env:PXR_PLUGINPATH_NAME = @(
    (Join-Path $Build "usd\usdGenSchema\resources"),
    (Join-Path $UsdInstallDir "lib\usd"),
    (Join-Path $UsdInstallDir "plugin\usd")
) -join ';'

$examples = Join-Path $Root "examples"
$maps = Join-Path $examples "maps"
New-Item -ItemType Directory -Force $maps | Out-Null

function Bake([string] $scene, [string] $mesh, [string] $out, [string[]] $arguments) {
    & $tool (Join-Path $examples $scene) $mesh (Join-Path $maps $out) @arguments
    if ($LASTEXITCODE -ne 0) { throw "usdGenBakePtex failed for $out" }
}

Bake "guide-interpolate-plane.usda" "/World/Skin" "guide_regions.ptx" `
    @("--res", "5", "--pattern", "voronoi", "--cells", "5", "--seed", "3", "--values", "index", "--jitter", "0.03")
Bake "clump-ptex-plane.usda" "/World/Skin" "clump_regions.ptx" `
    @("--res", "5", "--pattern", "voronoi", "--cells", "90", "--seed", "11", "--values", "random", "--jitter", "0.015")
Bake "clump-ptex-plane.usda" "/World/Skin" "clump_tightness.ptx" `
    @("--res", "4", "--pattern", "voronoi", "--cells", "6", "--seed", "5", "--values", "random")
