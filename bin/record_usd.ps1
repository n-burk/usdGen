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
    [switch] $ShowDomeLight
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

# usdrecord wants a "###" placeholder in the output name when frames are
# given; a negative -Frame renders the Default time into the plain name.
$args = @("--renderer", $Renderer, "--imageWidth", $Width)
if ($Frame -lt 0) { $args += "--defaultTime" } else { $args += @("--frames", $Frame) }
if ($Camera) { $args += @("--camera", $Camera) }
if ($Complexity) { $args += @("--complexity", $Complexity) }
if ($NoCameraLight) { $args += "--disableCameraLight" }
if ($ShowDomeLight) { $args += "--enableDomeLightVisibility" }
$args += @((Resolve-Path $Scene).Path, $Output)
& $python $usdrecord @args
exit $LASTEXITCODE
