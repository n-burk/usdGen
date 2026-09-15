param(
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
    (Join-Path $UsdInstallDir "plugin\usd"),
    (Join-Path $UsdInstallDir "lib\usd")
) | Where-Object { Test-Path $_ }
$env:PXR_PLUGINPATH_NAME = ($pluginDirs -join ';')

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

# The prefix's own usdview comes first in either form it ships: the
# extensionless python script (what usd-install has, and what the usdRig
# launchers run through PY) or a native/wrapper executable.
$candidates = @(
    (Join-Path $UsdInstallDir "bin\usdview"),
    (Join-Path $UsdInstallDir "bin\usdview.py"),
    (Join-Path $UsdInstallDir "bin\usdview.exe"),
    (Join-Path $UsdInstallDir "bin\usdview.cmd"),
    (Join-Path $UsdInstallDir "bin\usdview.bat")
) | Where-Object { Test-Path $_ }

$command = Get-Command usdview -ErrorAction SilentlyContinue | Select-Object -First 1
if ($candidates.Count -gt 0) {
    $usdview = $candidates[0]
} elseif ($command) {
    $usdview = $command.Source
} else {
    throw "usdview is not installed in '$UsdInstallDir' and is not on PATH. That prefix was built without Python/usdview; point USD at an OpenUSD build that has it, then launch this shortcut again."
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
