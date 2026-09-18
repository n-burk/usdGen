param(
    [string] $Label = "current",
    # Offline-only anti-aliasing reference: render at N x and box-downsample in
    # linear light. Cost goes as N^2 and it is NOT what the viewport does, so
    # label these renders as references (e.g. -Label aa1_ss4) and never compare
    # them to a real frame as if they were one.
    [ValidateRange(1, 8)]
    [int] $Supersample = 1,
    # Storm's MSAA sample count for the whole set (default 4 when unset).
    [int] $Msaa = 0
)

# Renders the UE-parity comparison set: both cameras of
# examples/head-hair-closeup.usda plus examples/styled-fur-plane.usda, into
# renders/ue-parity/<label>_<scene>_<camera>.png. Run with different -Label
# values (e.g. "baseline", then "current") to build before/after sets with
# one command each.
#
#   .\bin\render_ue_parity.ps1 -Label baseline

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OutDir = Join-Path $Root "renders\ue-parity"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$record = Join-Path $Root "bin\record_usd.ps1"
$examples = Join-Path $Root "examples"

function Render([string] $scene, [string] $sceneName, [string] $camera, [string] $cameraName, [switch] $NoCameraLight) {
    $out = Join-Path $OutDir "${Label}_${sceneName}_${cameraName}.png"
    # A HASHTABLE, not an array: splatting an array passes its elements
    # POSITIONALLY, so @("-Supersample", 4) would bind "-Supersample" to the
    # next free positional parameter (-Renderer) rather than naming anything.
    $extra = @{}
    if ($Supersample -gt 1) { $extra["Supersample"] = $Supersample }
    if ($Msaa -gt 0) { $extra["Msaa"] = $Msaa }
    if ($NoCameraLight) {
        & $record -Scene $scene -Output $out -Camera $camera -Width 1280 -Complexity veryhigh -NoCameraLight @extra
    } else {
        & $record -Scene $scene -Output $out -Camera $camera -Width 1280 -Complexity veryhigh @extra
    }
    if ($LASTEXITCODE -ne 0) { throw "record_usd.ps1 failed for $sceneName/$cameraName" }
    Write-Host "wrote $out"
}

# head-hair-closeup-render.usda sublayers head-hair-closeup.usda and adds
# back the backdrop sphere that scene deliberately leaves out (a 260-unit
# backdrop in the viewable scene blows out usdview's FreeCamera bbox-derived
# near/far and clips the hair on zoom -- see make_head_hair.py). Camera
# paths are the same either way since they come from the sublayer.
# head-hair-closeup now carries its own Key/Rim/Sky lights (see
# make_head_hair.py); -NoCameraLight so usdrecord's default headlight doesn't
# mask whether those scene lights actually do the work.
$headHair = Join-Path $examples "head-hair-closeup-render.usda"
Render $headHair "head-hair-closeup" "/World/TempleCam" "temple" -NoCameraLight
Render $headHair "head-hair-closeup" "/World/HeadCam" "head" -NoCameraLight

# styled-fur-plane authors no scene lights on purpose (its own header
# comment: usdview/usdrecord's camera headlight is the point), so it keeps
# the default headlight.
Render (Join-Path $examples "styled-fur-plane.usda") "styled-fur-plane" "/World/Cam" "cam"

# metahuman-hair-parity-render.usda sublayers metahuman-hair-parity.usda
# (which in turn sublayers examples/production/metahuman-hair/male_hair_01/hair.usda
# without modifying it) and adds the backdrop sphere, same reasoning as
# head-hair-closeup-render.usda. Its Groom/cameras/lights are NOT under
# /World (see make_metahuman_parity.py), so the camera paths here are at the
# stage root, not /World/TempleCam.
$metahuman = Join-Path $examples "metahuman-hair-parity-render.usda"
Render $metahuman "metahuman" "/TempleCam" "temple" -NoCameraLight
Render $metahuman "metahuman" "/HeadCam" "head" -NoCameraLight
