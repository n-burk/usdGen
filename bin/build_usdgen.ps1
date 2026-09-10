param(
    [switch] $Test,
    [string] $Build = "",
    [string] $UsdInstallDir = "",
    [string] $Generator = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
if (-not $Build) { $Build = Join-Path $Root "build" }
if (-not $UsdInstallDir) { $UsdInstallDir = Join-Path (Split-Path $Root) "OpenUSD_26_08" }

if (-not $Generator) {
    if (Get-Command ninja -ErrorAction SilentlyContinue) {
        $Generator = "Ninja"
    } else {
        $Generator = "Visual Studio 17 2022"
    }
}

$NeedsConfigure = -not (Test-Path (Join-Path $Build "CMakeCache.txt"))
if ($Generator -eq "Ninja" -and -not (Test-Path (Join-Path $Build "build.ninja"))) {
    $NeedsConfigure = $true
}
if ($NeedsConfigure) {
    cmake -S $Root -B $Build -G $Generator -D CMAKE_BUILD_TYPE=Release `
        -D USD_INSTALL_DIR=$UsdInstallDir -D USDGEN_WITH_RIGEXEC=OFF
}

cmake --build $Build --config Release --parallel
if ($Test) {
    ctest --test-dir $Build -L 'T0|T1' --output-on-failure --parallel
}
