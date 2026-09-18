param(
    [Parameter(Mandatory = $true)] [string] $Scene,
    [Parameter(Mandatory = $true)] [string] $Output,
    [string] $Camera = "",
    [int] $Width = 1024,
    [string] $Renderer = "GL",
    [double] $Frame = -1,
    # usdrecord's UsdAppUtils complexity preset (complexityArgs.py:40-43:
    # low 1.0, medium 1.1, high 1.2, veryhigh 1.3). It drives
    # UsdImagingGLEngine's refineLevel 0/1/2/3, which usdGen tiles now follow
    # like any native BasisCurves. Empty leaves usdrecord's own default.
    [ValidateSet("", "low", "medium", "high", "veryhigh")]
    [string] $Complexity = "",
    # Drop usdrecord's default headlight so only the stage's own lights (a
    # DomeLight, say) illuminate the frame -- the usdview state where the
    # camera light is off and "Enable Default Dome Light" is on.
    [switch] $NoCameraLight,
    # Draw the dome light's environment texture into the background instead of
    # using it for lighting only.
    [switch] $ShowDomeLight,
    # Render at N x the requested width and box-downsample back, averaging in
    # LINEAR light. This is an offline-only anti-aliasing reference: it is what
    # the frame converges to given unlimited spatial samples, so it separates
    # "the shader is wrong" from "this needs more samples or a temporal
    # filter". It is NOT a shipping mode -- cost goes as N^2.
    [ValidateRange(1, 8)]
    [int] $Supersample = 1,
    # Storm's MSAA sample count for this render (HdxTaskController reads
    # HDX_MSAA_SAMPLE_COUNT; the default is 4). The strand material's sample
    # mask scales with whatever it actually gets, so raising this raises the
    # number of coverage levels a sub-pixel strand can spend.
    [int] $Msaa = 0
)

# Headless render of a usdGen scene with the plugins from this build tree:
# the same environment bin\launch_usdview.ps1 sets up, handed to usdrecord.

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
$Build = Join-Path $Root "build"

$UsdInstallDir = $env:USD
if (-not $UsdInstallDir) {
    $UsdInstallDir = Join-Path (Split-Path $Root) "usdRig\usd-install"
}
if (-not (Test-Path $UsdInstallDir)) {
    throw "OpenUSD prefix '$UsdInstallDir' does not exist. Set USD to the install prefix that usdRig builds against."
}
$UsdInstallDir = (Resolve-Path $UsdInstallDir).Path
if (-not (Test-Path (Join-Path $Build "usd\usdGenSchema\resources\plugInfo.json"))) {
    throw "usdGen has not been built. Run .\bin\build_usdgen.ps1 first."
}

$pluginDirs = @(
    (Join-Path $Build "usd\usdGenSchema\resources"),
    (Join-Path $Build "usd\usdGenImaging\resources"),
    (Join-Path $Build "usd\usdGenShaders\resources"),
    (Join-Path $Build "usd\usdGenTools\resources"),
    (Join-Path $UsdInstallDir "plugin\usd"),
    (Join-Path $UsdInstallDir "lib\usd")
) | Where-Object { Test-Path $_ }
$env:PXR_PLUGINPATH_NAME = ($pluginDirs -join ';')

# Storm's MSAA sample count. The default is 4, which gives a sub-pixel strand
# only 5 coverage levels to spend and leaves the coat speckled; 8 removes most
# of that for about 13% of frame time, where 16 costs 65% for almost nothing
# more (head-hair-closeup at 1280: 14.84 / 16.71 / 24.52 ms, isolated-pixel
# spike score 0.00631 / 0.00536 / 0.00530 against 0.00176 for a 4x
# supersampled reference). This is launcher configuration, not an OpenUSD
# patch -- HdxTaskController reads it -- so set it only if the caller has not.
if (-not $env:HDX_MSAA_SAMPLE_COUNT) { $env:HDX_MSAA_SAMPLE_COUNT = "8" }


$CudaBinDir = $null
$CachePath = Join-Path $Build "CMakeCache.txt"
if (Test-Path $CachePath) {
    $cudaCompilerLine = Get-Content $CachePath | Where-Object { $_ -match '^CMAKE_CUDA_COMPILER:' } | Select-Object -First 1
    if ($cudaCompilerLine) {
        $candidate = Split-Path $cudaCompilerLine.Split('=', 2)[1]
        if (Test-Path $candidate) { $CudaBinDir = $candidate }
    }
}
$runtimeDirs = @($Build, $CudaBinDir, (Join-Path $UsdInstallDir "bin"), (Join-Path $UsdInstallDir "lib")) |
    Where-Object { $_ -and (Test-Path $_) }
$env:PATH = (($runtimeDirs -join ';') + ';' + $env:PATH)

$pythonDirs = @(
    (Join-Path $Build "python"),
    (Join-Path $UsdInstallDir "Lib\site-packages"),
    (Join-Path $UsdInstallDir "lib\python")
) | Where-Object { Test-Path $_ }
$env:PYTHONPATH = ($pythonDirs -join ';') + $(if ($env:PYTHONPATH) { ';' + $env:PYTHONPATH } else { '' })

$usdrecord = Join-Path $UsdInstallDir "bin\usdrecord"
if (-not (Test-Path $usdrecord)) { throw "usdrecord is not installed in '$UsdInstallDir'." }
$python = $env:PY
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
}
if (-not $python) { throw "Python is not on PATH. Set PY to the interpreter that imports this build's pxr." }

if ($Msaa -gt 0) { $env:HDX_MSAA_SAMPLE_COUNT = "$Msaa" }

# Supersampling renders wide into a scratch file and box-downsamples after.
$RenderWidth = $Width * $Supersample
$RenderTarget = $Output
if ($Supersample -gt 1) {
    $RenderTarget = [System.IO.Path]::ChangeExtension($Output, ".ss$Supersample.png")
}

# usdrecord wants a "###" placeholder in the output name when frames are
# given; a negative -Frame renders the Default time into the plain name.
$recordArgs = @("--renderer", $Renderer, "--imageWidth", $RenderWidth)
if ($Frame -lt 0) { $recordArgs += "--defaultTime" } else { $recordArgs += @("--frames", $Frame) }
if ($Camera) { $recordArgs += @("--camera", $Camera) }
if ($Complexity) { $recordArgs += @("--complexity", $Complexity) }
if ($NoCameraLight) { $recordArgs += "--disableCameraLight" }
if ($ShowDomeLight) { $recordArgs += "--enableDomeLightVisibility" }
$recordArgs += @((Resolve-Path $Scene).Path, $RenderTarget)
& $python $usdrecord @recordArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Supersample -gt 1) {
    # Box-average in linear light, then re-encode. Averaging sRGB values
    # directly darkens every edge -- which would make the reference wrong in
    # exactly the place we are trying to measure.
    $downsample = Join-Path $PSScriptRoot "downsample_linear.py"
    & $python $downsample $RenderTarget $Output $Supersample
    if ($LASTEXITCODE -ne 0) { throw "linear downsample failed" }
    Remove-Item $RenderTarget -Force
}
exit 0
