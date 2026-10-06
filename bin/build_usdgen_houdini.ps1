# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
# Configure, build, install, and optionally test usdGen against the OpenUSD
# vendored inside a Houdini install, in build-houdini. The Houdini twin of
# build_usdgen.ps1: same generator, same build type, but USD comes from
# cmake\houdini\pxrConfig.cmake (headers in toolkit\include, import libraries
# in custom\houdini\dsolib) instead of a stock OpenUSD install. The default
# stock-USD build is untouched.
#
# Usage: bin\build_usdgen_houdini.ps1 [-Test] [-Build <dir>]
#   -Test runs ctest -L '^T[01]$' after the build (and configures
#   USDGEN_BUILD_TESTS=ON to make that possible).
param(
    [switch] $Test,
    [string] $Build = "",
    [string] $HoudiniRoot = "",
    [int] $Jobs = 8
)

$ErrorActionPreference = "Stop"
if (-not $HoudiniRoot) { $HoudiniRoot = $env:HOUDINI_ROOT }
if (-not $HoudiniRoot) { $HoudiniRoot = $env:HFS }
if (-not $HoudiniRoot) {
    throw "No Houdini root: pass -HoudiniRoot (or set HOUDINI_ROOT/HFS) with the Houdini install whose vendored OpenUSD should be used."
}
if (-not (Test-Path (Join-Path $HoudiniRoot "bin\hython.exe"))) {
    throw "No Houdini install at '$HoudiniRoot'."
}
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
if (-not $Build) { $Build = Join-Path $Root "build-houdini" }
$Shim = Join-Path $Root "cmake\houdini"
if (-not (Test-Path (Join-Path $Shim "pxrConfig.cmake"))) {
    throw "Houdini pxr shim not found at '$Shim\pxrConfig.cmake'."
}

# A configured build must use the Visual Studio installation recorded in its
# CMake cache (see build_usdgen.ps1 for why); otherwise discover one.
$CachedVsDevCmd = $null
$CachedVCToolsDir = $null
$CachedBuildTests = $null
$CachePath = Join-Path $Build "CMakeCache.txt"
if (Test-Path $CachePath) {
    $cacheLines = Get-Content $CachePath
    $cachedCompilerLine = $cacheLines | Where-Object {
        $_ -match '^CMAKE_CXX_COMPILER:'
    } | Select-Object -First 1
    if ($cachedCompilerLine) {
        $cachedCompiler = $cachedCompilerLine.Split('=', 2)[1]
        $cachedVsRoot = $cachedCompiler -replace '[\\/]VC[\\/].*$', ''
        $CachedVCToolsDir = $cachedCompiler -replace '[\\/]bin[\\/].*$', ''
        if ($cachedVsRoot -and $cachedVsRoot -ne $cachedCompiler) {
            $candidate = Join-Path $cachedVsRoot "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) { $CachedVsDevCmd = $candidate }
        }
    }
    $cachedTestsLine = $cacheLines | Where-Object {
        $_ -match '^USDGEN_BUILD_TESTS:'
    } | Select-Object -First 1
    if ($cachedTestsLine) { $CachedBuildTests = $cachedTestsLine.Split('=', 2)[1] }
}

$VsDevCmd = $null
$activeVCToolsDir = if ($env:VCToolsInstallDir) {
    $env:VCToolsInstallDir.TrimEnd('\', '/')
} else {
    $null
}
$cacheToolchainIsActive = -not $CachedVCToolsDir -or
    ($activeVCToolsDir -and $activeVCToolsDir -eq $CachedVCToolsDir)
if (-not $cacheToolchainIsActive -or
    -not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    -not $env:VSCMD_VER -or -not $env:WindowsSdkDir) {
    if ($CachedVsDevCmd) { $VsDevCmd = $CachedVsDevCmd }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not $VsDevCmd -and (Test-Path $vswhere)) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installationPath) {
            $candidate = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) { $VsDevCmd = $candidate }
        }
    }
    if (-not $VsDevCmd) {
        throw "MSVC/Windows SDK environment is unavailable. Open a Developer PowerShell for Visual Studio or install the C++ build tools."
    }
}

function Invoke-CMake([string[]] $Arguments) {
    if (-not $VsDevCmd) {
        & cmake @Arguments
        if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }
        return
    }
    $quotedArguments = ($Arguments | ForEach-Object {
        '"' + ($_ -replace '"', '\\"') + '"'
    }) -join ' '
    $cmakePath = (Get-Command cmake -ErrorAction Stop).Source
    $commandLine = "set `"NoDefaultCurrentDirectoryInExePath=`" && call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && `"$cmakePath`" $quotedArguments"
    & cmd.exe /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }
}

$NeedsConfigure = -not (Test-Path (Join-Path $Build "build.ninja"))
if ($Test -and $CachedBuildTests -ne 'ON') {
    $NeedsConfigure = $true
}
if ($NeedsConfigure) {
    # The Python test scripts run under $ENV{PY} baked in at configure time
    # and import pxr. Houdini's own interpreter already imports its vendored
    # pxr natively, so default PY to it (an explicit PY still wins).
    if (-not $env:PY) {
        $env:PY = Join-Path $HoudiniRoot "python313\python.exe"
    }
    # Forward slashes: CMake takes a backslash in a -D value as an escape.
    $shimFlag = $Shim.Replace('\', '/')
    $hfsFlag = $HoudiniRoot.Replace('\', '/')
    $configureArguments = @('-S', $Root, '-B', $Build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DUSD_INSTALL_DIR=$shimFlag",
        "-DHOUDINI_ROOT=$hfsFlag", "-DUSDGEN_TEST_USD_DIR=$hfsFlag",
        '-DUSDGEN_WITH_RIGEXEC=OFF', '-DUSDGEN_ENABLE_CUDA=OFF',
        '-DUSDGEN_ENABLE_VULKAN_RUNTIME=ON', '-DUSDGEN_ENABLE_VULKAN=ON',
        "-DCMAKE_INSTALL_PREFIX=$($Build.Replace('\', '/'))/install")
    if ($Test) { $configureArguments += '-DUSDGEN_BUILD_TESTS=ON' }
    Invoke-CMake $configureArguments
}

# The groom translation units are as heavy as rigExec's; Ninja's CPU-count
# default OOMs the compiler, so cap the parallelism (-Jobs to override).
Invoke-CMake @('--build', $Build, '--config', 'Release', '--parallel', "$Jobs")
Invoke-CMake @('--install', $Build)
if ($Test) {
    ctest --test-dir $Build -C Release -L '^T[01]$' --output-on-failure --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "ctest failed with exit code $LASTEXITCODE" }
}
