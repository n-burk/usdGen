# Only named parameters bind by name; everything else (the scene path and any
# usdview flags) falls through to $UsdviewArgs.
[CmdletBinding(PositionalBinding = $false)]
param(
    # Render the viewport at N x the window and box-downsample it in linear
    # light on present (usdGenTools; see docs/storm-fur.md). Cost goes as N^2,
    # so this is a look-dev switch: 2 is the useful one, 4 is a hero still.
    # It can also be changed in the usdGen > Viewport Supersampling menu.
    [ValidateRange(1, 8)]
    [int] $Supersample = 0,
    # Run the prefix's testusdview on this script instead of opening
    # usdview interactively. The environment is the same either way,
    # which is the point: the T3 tonic scripts and the workspace
    # screenshot need exactly the plugin, python and DLL paths this
    # launcher already assembles.
    [string] $TestScript = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $UsdviewArgs
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
$Build = Join-Path $Root "build"

# The OpenUSD prefix the usdRig helpers target (../usdRig/bin/_env.bat resolves
# USD as usdRig/../usd-install). That build ships Python, pxr and usdview; the
# older sibling OpenUSD_26_08 prefix has no usdview at all. USD in the
# environment overrides, as it does for the usdRig scripts.
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

# Plug consumes a semicolon-delimited list on Windows. Put the build-tree
# resources first so this launcher uses the plugins just built here.
$pluginDirs = @(
    (Join-Path $Build "usd\usdGenSchema\resources"),
    (Join-Path $Build "usd\usdGenImaging\resources"),
    (Join-Path $Build "usd\usdGenShaders\resources"),
    (Join-Path $Build "usd\usdGenTools\resources"),
    (Join-Path $Build "usd\usdGenTonic\resources"),
    (Join-Path $Build "usd\usdGenTonicTools\resources"),
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

# Viewport supersampling, read by the usdGenTools usdview plugin. -Supersample
# wins over the environment; without either, the viewport renders 1:1.
if ($Supersample -gt 0) { $env:USDGEN_USDVIEW_SUPERSAMPLE = "$Supersample" }


# A CUDA-enabled usdGen.dll imports cudart/cusolver from the bin directory of
# the toolkit it was compiled against. That is not necessarily the toolkit on
# PATH (a host can carry several, and PATH often names an older one whose
# cudart has a different soname), so take it from the build that is about to
# be loaded: the configure recorded its nvcc, and the DLLs sit beside it.
$CudaBinDir = $null
$CachePath = Join-Path $Build "CMakeCache.txt"
if (Test-Path $CachePath) {
    $cudaCompilerLine = Get-Content $CachePath | Where-Object {
        $_ -match '^CMAKE_CUDA_COMPILER:'
    } | Select-Object -First 1
    if ($cudaCompilerLine) {
        $candidate = Split-Path $cudaCompilerLine.Split('=', 2)[1]
        if (Test-Path $candidate) { $CudaBinDir = $candidate }
    }
}

# usdGen and OpenUSD DLLs must be discoverable before Python imports pxr.
$runtimeDirs = @($Build, $CudaBinDir, (Join-Path $UsdInstallDir "bin"), (Join-Path $UsdInstallDir "lib")) |
    Where-Object { $_ -and (Test-Path $_) }
$env:PATH = (($runtimeDirs -join ';') + ';' + $env:PATH)

# usdview is a Python script: pxr has to be importable. A Windows OpenUSD
# install keeps its modules in Lib\site-packages, a build configured the POSIX
# way under lib\python; both are added when both exist rather than one being
# assumed (this is what _env.bat does on the usdRig side). build\python is
# where usdGen stages its own python package.
$pythonDirs = @(
    (Join-Path $Build "python"),
    (Join-Path $UsdInstallDir "Lib\site-packages"),
    (Join-Path $UsdInstallDir "lib\python")
) | Where-Object { Test-Path $_ }
$env:PYTHONPATH = ($pythonDirs -join ';') +
    $(if ($env:PYTHONPATH) { ';' + $env:PYTHONPATH } else { '' })

# The prefix's own viewer comes first in either form it ships: the
# extensionless python script (what usd-install has, and what the usdRig
# launchers run through PY) or a native/wrapper executable.
$viewer = if ($TestScript) { "testusdview" } else { "usdview" }
if ($TestScript) {
    if (-not (Test-Path $TestScript)) {
        throw "No such test script: '$TestScript'."
    }
    $UsdviewArgs = @("--testScript", (Resolve-Path $TestScript).Path) + $UsdviewArgs
}
$candidates = @(
    (Join-Path $UsdInstallDir "bin\$viewer"),
    (Join-Path $UsdInstallDir "bin\$viewer.py"),
    (Join-Path $UsdInstallDir "bin\$viewer.exe"),
    (Join-Path $UsdInstallDir "bin\$viewer.cmd"),
    (Join-Path $UsdInstallDir "bin\$viewer.bat")
) | Where-Object { Test-Path $_ }

$command = Get-Command $viewer -ErrorAction SilentlyContinue | Select-Object -First 1
if ($candidates.Count -gt 0) {
    $usdview = $candidates[0]
} elseif ($command) {
    $usdview = $command.Source
} else {
    throw "$viewer is not installed in '$UsdInstallDir' and is not on PATH. That prefix was built without Python/usdview; point USD at an OpenUSD build that has it, then launch this shortcut again."
}

# A script has to be handed to an interpreter; only an .exe/.cmd/.bat runs on
# its own. PY names the interpreter that can import this build's pxr, the same
# override the usdRig helpers honour.
$extension = [IO.Path]::GetExtension($usdview)
if ($extension -eq '.exe' -or $extension -eq '.cmd' -or $extension -eq '.bat') {
    & $usdview @UsdviewArgs
} else {
    $python = $env:PY
    if (-not $python) {
        $python = Get-Command python -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty Source
    }
    if (-not $python) { throw "usdview is a Python script, but Python is not on PATH. Set PY to the interpreter that imports this build's pxr." }
    & $python $usdview @UsdviewArgs
}
exit $LASTEXITCODE
