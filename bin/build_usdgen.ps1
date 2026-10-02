# Configure and build; -Vulkan -Test also builds and runs the Vulkan tests.
# Vulkan SDK discovery uses VULKAN_SDK or the system toolchain via CMake.
param(
    [switch] $Test,
    [string] $Build = "",
    [string] $UsdInstallDir = "",
    [string] $Generator = "",
    [switch] $Cuda,
    [switch] $NoCuda,
    [string] $CudaToolkitDir = "",
    [switch] $Vulkan,
    [switch] $NoVulkan
)

$ErrorActionPreference = "Stop"
if ($Cuda -and $NoCuda) { throw "-Cuda and -NoCuda are mutually exclusive." }
if ($Vulkan -and $NoVulkan) { throw "-Vulkan and -NoVulkan are mutually exclusive." }
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
if (-not $Build) { $Build = Join-Path $Root "build" }

# Default to the OpenUSD prefix the usdRig helpers target (../usdRig/bin/_env.bat
# resolves USD as usdRig/../usd-install): that build ships Python and usdview,
# which the older sibling OpenUSD_26_08 prefix does not. USD in the environment
# wins over the default, exactly as it does for the usdRig scripts, and an
# explicit -UsdInstallDir wins over both.
if (-not $UsdInstallDir) { $UsdInstallDir = $env:USD }
if (-not $UsdInstallDir) {
    $UsdInstallDir = Join-Path (Split-Path $Root) "usdRig\usd-install"
}
if (-not (Test-Path $UsdInstallDir)) {
    throw "OpenUSD prefix '$UsdInstallDir' does not exist. Supply -UsdInstallDir (or set USD) with the Windows OpenUSD install prefix that usdRig builds against."
}
$UsdInstallDir = (Resolve-Path $UsdInstallDir).Path

# A configured build must use the Visual Studio installation recorded in its
# CMake cache.  Mixing a VS 2019 compiler with VS 2022 STL headers produces
# STL1001, while a plain PowerShell has neither set of include paths.
$CachedVsDevCmd = $null
$CachedVCToolsDir = $null
$CachedUsdInstallDir = $null
$CachedBuildTests = $null
$CachedEnableCuda = $null
$CachedCudaCompiler = $null
$CachedEnableVulkan = $null
$CachedBuildVulkanTests = $null
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
    $cachedUsdLine = $cacheLines | Where-Object {
        $_ -match '^USD_INSTALL_DIR:'
    } | Select-Object -First 1
    if ($cachedUsdLine) { $CachedUsdInstallDir = $cachedUsdLine.Split('=', 2)[1] }
    $cachedTestsLine = $cacheLines | Where-Object {
        $_ -match '^USDGEN_BUILD_TESTS:'
    } | Select-Object -First 1
    if ($cachedTestsLine) { $CachedBuildTests = $cachedTestsLine.Split('=', 2)[1] }
    $cachedCudaLine = $cacheLines | Where-Object {
        $_ -match '^USDGEN_ENABLE_CUDA:'
    } | Select-Object -First 1
    if ($cachedCudaLine) { $CachedEnableCuda = $cachedCudaLine.Split('=', 2)[1] }
    $cachedNvccLine = $cacheLines | Where-Object {
        $_ -match '^CMAKE_CUDA_COMPILER:'
    } | Select-Object -First 1
    if ($cachedNvccLine) { $CachedCudaCompiler = $cachedNvccLine.Split('=', 2)[1] }
    $cachedVulkanLine = $cacheLines | Where-Object {
        $_ -match '^USDGEN_ENABLE_VULKAN_RUNTIME:'
    } | Select-Object -First 1
    if ($cachedVulkanLine) { $CachedEnableVulkan = $cachedVulkanLine.Split('=', 2)[1] }
    $cachedVulkanTestsLine = $cacheLines | Where-Object {
        $_ -match '^USDGEN_BUILD_VULKAN_TESTS:'
    } | Select-Object -First 1
    if ($cachedVulkanTestsLine) { $CachedBuildVulkanTests = $cachedVulkanTestsLine.Split('=', 2)[1] }
}

# USD_INSTALL_DIR only takes effect at configure time, and find_package caches
# pxr_DIR beside it. Re-configuring a tree over a different prefix leaves the
# two disagreeing, so say so rather than silently building against the old USD.
if ($CachedUsdInstallDir) {
    $normalize = { param($p) $p.Replace('\', '/').TrimEnd('/') }
    if ((& $normalize $CachedUsdInstallDir) -ne (& $normalize $UsdInstallDir)) {
        throw "'$Build' is configured against '$CachedUsdInstallDir', not '$UsdInstallDir'. Delete the build directory (or pass -Build with a fresh one) to retarget it."
    }
}

# A Ninja/MSVC build needs the MSVC and Windows SDK environment. Developers
# often invoke this wrapper from ordinary PowerShell, where cl.exe may be on
# PATH but the SDK import libraries are not. Re-enter through VsDevCmd when
# needed so the wrapper works from either ordinary or Developer PowerShell.
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
        $candidate = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat"
        if (Test-Path $candidate) { $VsDevCmd = $candidate }
    }
    if (-not $VsDevCmd) {
        throw "MSVC/Windows SDK environment is unavailable. Open a Developer PowerShell for Visual Studio or install the C++ build tools."
    }
}

# USDGEN_ENABLE_CUDA compiles the .cu execution backend. Without it usdGen
# still builds, but every commit that asks for the CUDA backend is refused at
# runtime with "CUDA support was not enabled in this build", so enable it
# whenever this host can actually run the result.
#
# CMake's own search takes the first nvcc on PATH, which on a host carrying
# several toolkits is rarely the right one: an 11.x nvcc rejects a modern MSVC
# outright (crt/host_config.h pins an upper _MSC_VER and stops with #error),
# and CUDA_PATH is often left pointing at a version that has since been
# uninstalled. Resolve the toolkit here instead and hand CMake the compiler.

function Get-ActiveMsvcVer {
    # _MSC_VER for the toolset this build will use: MSVC 14.43 -> 1943, which
    # is the number the CUDA headers compare against.
    $toolsDir = $env:VCToolsInstallDir
    if (-not $toolsDir) { $toolsDir = $CachedVCToolsDir }
    if (-not $toolsDir -and $VsDevCmd) {
        # <VS root>\Common7\Tools\VsDevCmd.bat -> <VS root>\VC\Tools\MSVC\<ver>
        $msvcRoot = Join-Path (Split-Path (Split-Path (Split-Path $VsDevCmd))) "VC\Tools\MSVC"
        if (Test-Path $msvcRoot) {
            $toolsDir = Get-ChildItem $msvcRoot -Directory |
                Sort-Object { [version] $_.Name } |
                Select-Object -Last 1 -ExpandProperty FullName
        }
    }
    if (-not $toolsDir) { return $null }
    if ((Split-Path $toolsDir.TrimEnd('\', '/') -Leaf) -match '^(\d+)\.(\d+)') {
        return 1900 + (100 * ([int] $Matches[1] - 14)) + [int] $Matches[2]
    }
    return $null
}

function Get-CudaHostMsvcRange([string] $root) {
    # crt/host_config.h states the supported window as a single guard,
    #   #if _MSC_VER < 1910 || _MSC_VER >= 1950
    # so read the bounds rather than discovering them from a wall of nvcc
    # #error output halfway through a configure.
    $header = Join-Path $root "include\crt\host_config.h"
    if (-not (Test-Path $header)) { return $null }
    $found = Select-String -Path $header -ErrorAction SilentlyContinue `
        -Pattern '_MSC_VER\s*<\s*(\d+)\s*\|\|\s*_MSC_VER\s*>=\s*(\d+)' |
        Select-Object -First 1
    if (-not $found) { return $null }
    return @{
        Min = [int] $found.Matches[0].Groups[1].Value
        Max = [int] $found.Matches[0].Groups[2].Value
    }
}

function Get-CudaToolkits {
    $roots = New-Object System.Collections.Generic.List[string]
    if ($CudaToolkitDir) {
        # An explicitly named toolkit is the whole candidate set: the caller
        # picked it, so it must not lose the "newest wins" sort below to some
        # other toolkit that happens to be installed.
        if (-not (Test-Path $CudaToolkitDir)) {
            throw "-CudaToolkitDir '$CudaToolkitDir' does not exist."
        }
        $roots.Add($CudaToolkitDir)
    } else {
        foreach ($fromEnv in @($env:CUDA_PATH, $env:CUDA_HOME)) {
            if ($fromEnv) { $roots.Add($fromEnv) }
        }
        $nvccOnPath = Get-Command nvcc.exe -ErrorAction SilentlyContinue
        if ($nvccOnPath) { $roots.Add((Split-Path (Split-Path $nvccOnPath.Source))) }
        $installRoot = Join-Path $env:ProgramFiles "NVIDIA GPU Computing Toolkit\CUDA"
        if (Test-Path $installRoot) {
            foreach ($dir in Get-ChildItem $installRoot -Directory) { $roots.Add($dir.FullName) }
        }
    }

    $seen = @{}
    $toolkits = @()
    foreach ($root in $roots) {
        if (-not $root -or -not (Test-Path $root)) { continue }
        $resolved = (Resolve-Path $root).Path.TrimEnd('\', '/')
        if ($seen.ContainsKey($resolved.ToLowerInvariant())) { continue }
        $seen[$resolved.ToLowerInvariant()] = $true
        $nvcc = Join-Path $resolved "bin\nvcc.exe"
        if (-not (Test-Path $nvcc)) { continue }
        $release = $null
        try {
            $release = & $nvcc --version | Select-String 'release (\d+)\.(\d+)' |
                Select-Object -First 1
        } catch { $release = $null }
        if (-not $release) { continue }
        $toolkits += [pscustomobject] @{
            Root      = $resolved
            Nvcc      = $nvcc
            Version   = [version] ("{0}.{1}" -f $release.Matches[0].Groups[1].Value,
                                                $release.Matches[0].Groups[2].Value)
            MsvcRange = Get-CudaHostMsvcRange $resolved
        }
    }
    return $toolkits
}

$CudaToolkit = $null
$CudaRejected = @()
if (-not $NoCuda) {
    $ActiveMsvcVer = Get-ActiveMsvcVer
    $usable = @()
    foreach ($toolkit in (Get-CudaToolkits)) {
        $range = $toolkit.MsvcRange
        if ($ActiveMsvcVer -and $range -and
            ($ActiveMsvcVer -lt $range.Min -or $ActiveMsvcVer -ge $range.Max)) {
            $CudaRejected += "CUDA $($toolkit.Version) ('$($toolkit.Root)') accepts _MSC_VER $($range.Min)-$($range.Max - 1); this toolset is $ActiveMsvcVer"
            continue
        }
        $usable += $toolkit
    }
    # Newest usable toolkit: it knows the most recent architectures, which is
    # what the CMake arch probe needs for a current card.
    $CudaToolkit = $usable | Sort-Object Version | Select-Object -Last 1
}

# The CMake arch probe asks nvidia-smi which sm_ to build for, so an answer
# from it is the same precondition there as here. Auto-detection keeps a
# GPU-less or toolkit-less host configuring green on the CPU-only engine;
# -Cuda turns a missing toolkit into an error instead, -NoCuda opts out.
$EnableCuda = $false
if ($NoCuda) {
    $EnableCuda = $false
} elseif ($Cuda -or $CudaToolkitDir) {
    if (-not $CudaToolkit) {
        $detail = if ($CudaRejected) { " Rejected: " + ($CudaRejected -join '; ') + "." } else { "" }
        throw "CUDA was requested but no usable CUDA toolkit was found.$detail Install one (or pass -CudaToolkitDir with its root)."
    }
    $EnableCuda = $true
} elseif ($CudaToolkit) {
    $smi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if ($smi) {
        $computeCaps = $null
        try {
            $computeCaps = & $smi.Source --query-gpu=compute_cap --format=csv,noheader
        } catch { $computeCaps = $null }
        $EnableCuda = [bool] ($computeCaps -match '\d+\.\d+')
    }
}

# CMake will not swap the CUDA compiler of a configured tree -- the cached
# CMAKE_CUDA_COMPILER wins and a newly installed toolkit would be ignored in
# silence, which is exactly the case that matters here (a tree first configured
# against an older toolkit than the sources need). Say so, as the USD prefix
# check above does, rather than building the wrong thing.
if ($EnableCuda -and $CachedCudaCompiler) {
    $normalizeNvcc = { param($p) $p.Replace('\', '/').TrimEnd('/').ToLowerInvariant() }
    if ((& $normalizeNvcc $CachedCudaCompiler) -ne (& $normalizeNvcc $CudaToolkit.Nvcc)) {
        throw "'$Build' is configured against the CUDA compiler '$CachedCudaCompiler', but the toolkit selected now is '$($CudaToolkit.Nvcc)' (CUDA $($CudaToolkit.Version)). CMake cannot retarget a configured tree's CUDA compiler: delete the build directory (or pass -Build with a fresh one) to pick the new toolkit up."
    }
}

if ($EnableCuda) {
    Write-Host "usdGen: CUDA backend ON (toolkit $($CudaToolkit.Version) at '$($CudaToolkit.Root)')"
} else {
    $why = if ($NoCuda) { "-NoCuda" }
        elseif (-not $CudaToolkit) { "no usable CUDA toolkit" }
        else { "no CUDA GPU reported by nvidia-smi" }
    Write-Host "usdGen: CUDA backend OFF ($why); the CUDA execution backend will be unavailable at runtime."
    foreach ($rejection in $CudaRejected) { Write-Host "  skipped: $rejection" }
}

# The prefix root is where an installed OpenUSD keeps pxrConfig.cmake; the
# recursive search is the fallback for a staged or nested layout. Looking at
# the root first also keeps the search off a prefix that carries its own
# source and build trees, as usd-install does.
$PXRConfig = $null
$rootConfig = Join-Path $UsdInstallDir "pxrConfig.cmake"
if (Test-Path $rootConfig) {
    $PXRConfig = $rootConfig
} else {
    $PXRConfig = Get-ChildItem -Path $UsdInstallDir -Filter pxrConfig.cmake -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $PXRConfig) {
    throw "OpenUSD was not found under '$UsdInstallDir'. Supply -UsdInstallDir with a Windows OpenUSD install prefix containing pxrConfig.cmake."
}

if (-not $Generator) {
    if (Get-Command ninja -ErrorAction SilentlyContinue) {
        $Generator = "Ninja"
    } else {
        $Generator = "Visual Studio 17 2022"
    }
}

function Invoke-CMake([string[]] $Arguments) {
    if (-not $VsDevCmd) {
        & cmake @Arguments
        if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }
        return
    }

    # CMake's argument parsing occurs in cmd.exe in this branch. All values
    # emitted by this wrapper are quoted, preserving spaces in VS, the source
    # checkout, build directory, and OpenUSD prefix.
    $quotedArguments = ($Arguments | ForEach-Object {
        '"' + ($_ -replace '"', '\\"') + '"'
    }) -join ' '
    $cmakePath = (Get-Command cmake -ErrorAction Stop).Source
    # VsDevCmd.bat resolves vswhere.exe by pushd'ing into the VS Installer
    # directory and then invoking it unqualified. When
    # NoDefaultCurrentDirectoryInExePath is set in the parent environment (a
    # number of terminals and CI agents set it), cmd.exe does not search the
    # current directory, that lookup fails, and the batch file prints an
    # alarming "'vswhere.exe' is not recognized" on stderr for every configure
    # and build. Clearing it for this child process alone restores the lookup;
    # nothing in the build itself reads the variable.
    $commandLine = "set `"NoDefaultCurrentDirectoryInExePath=`" && call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && `"$cmakePath`" $quotedArguments"
    & cmd.exe /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }
}

$NeedsConfigure = -not (Test-Path (Join-Path $Build "CMakeCache.txt"))
if ($Generator -eq "Ninja" -and -not (Test-Path (Join-Path $Build "build.ninja"))) {
    $NeedsConfigure = $true
}
# -Test must also build the tests. USDGEN_BUILD_TESTS defaults OFF on Windows
# (only the portable subset builds there), so an existing tree that was
# configured without it has to be reconfigured before ctest can find anything.
if ($Test -and $CachedBuildTests -ne 'ON') {
    $NeedsConfigure = $true
}
# USDGEN_ENABLE_CUDA decides which sources the targets carry, so a tree
# configured the other way has to be re-configured before the switch takes.
$DesiredEnableCuda = if ($EnableCuda) { 'ON' } else { 'OFF' }
if ($CachedEnableCuda -and $CachedEnableCuda -ne $DesiredEnableCuda) {
    $NeedsConfigure = $true
}
# No Vulkan switch preserves CMake's first-configure auto-detection and any
# cached choice. Explicit switches also work on already-configured trees.
$DesiredEnableVulkan = if ($Vulkan) { 'ON' } elseif ($NoVulkan) { 'OFF' } else { $null }
$DesiredBuildVulkanTests = if ($Vulkan -and $Test) { 'ON' } elseif ($NoVulkan) { 'OFF' } else { $null }
if ($DesiredEnableVulkan -and $CachedEnableVulkan -ne $DesiredEnableVulkan) {
    $NeedsConfigure = $true
}
if ($DesiredBuildVulkanTests -and $CachedBuildVulkanTests -ne $DesiredBuildVulkanTests) {
    $NeedsConfigure = $true
}
if ($NeedsConfigure) {
    $configureArguments = @('-S', $Root, '-B', $Build, '-G', $Generator,
        '-DCMAKE_BUILD_TYPE=Release', "-DUSD_INSTALL_DIR=$UsdInstallDir",
        '-DUSDGEN_WITH_RIGEXEC=OFF', "-DUSDGEN_ENABLE_CUDA=$DesiredEnableCuda")
    if ($EnableCuda) {
        # Forward slashes: CMake takes a backslash in a -D value as an escape.
        $configureArguments += @(
            "-DCMAKE_CUDA_COMPILER=$($CudaToolkit.Nvcc.Replace('\', '/'))",
            "-DCUDAToolkit_ROOT=$($CudaToolkit.Root.Replace('\', '/'))")
    }
    if ($Test) { $configureArguments += '-DUSDGEN_BUILD_TESTS=ON' }
    if ($DesiredEnableVulkan) {
        # Keep the compatibility alias in sync when toggling a cached build.
        $configureArguments += @(
            "-DUSDGEN_ENABLE_VULKAN_RUNTIME=$DesiredEnableVulkan",
            "-DUSDGEN_ENABLE_VULKAN=$DesiredEnableVulkan")
    }
    if ($DesiredBuildVulkanTests) {
        $configureArguments += "-DUSDGEN_BUILD_VULKAN_TESTS=$DesiredBuildVulkanTests"
    }
    Invoke-CMake $configureArguments
}

Invoke-CMake @('--build', $Build, '--config', 'Release', '--parallel')
if ($Test) {
    $testLabels = if ($Vulkan) { '^(T0|T1|vulkan)$' } else { '^T[01]$' }
    ctest --test-dir $Build -C Release -L $testLabels --output-on-failure --parallel
    if ($LASTEXITCODE -ne 0) { throw "ctest failed with exit code $LASTEXITCODE" }
}
