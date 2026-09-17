param(
    [Parameter(Mandatory = $true)] [string] $Scene,
    # Output folder for the trace files (default: build\traces\<scene>).
    [string] $OutDir = "",
    # first:last; default the stage's start/end time codes.
    [string] $Frames = "",
    [int] $Loops = 2,
    # Read every published tile back like a renderer sync (engine harness).
    [switch] $Pull,
    # Only the per-frame table, no per-cook/per-operator lines.
    [switch] $Quiet,
    # Trace real usdview playback (testusdview + Storm) instead of the engine
    # harness. Needs a display.
    [switch] $Usdview,
    # -Usdview only: low | medium | high | veryhigh.
    [string] $Complexity = "high",
    # -Usdview only: "chrome" (usdview_trace.json) or "trace" (OpenUSD's
    # aggregate tree, usdview_trace.txt; far smaller than a Storm Chrome trace).
    [ValidateSet("chrome", "trace")]
    [string] $TraceFormat = "chrome"
)

# Profiles playback of a usdGen scene and writes a Chrome trace.
#
#   .\bin\trace_playback.ps1 -Scene examples\rbf-guides-plane.usda -Pull
#   .\bin\trace_playback.ps1 -Scene examples\rbf-guides-plane.usda -Usdview
#
# Engine harness (default): build\usdGenTracePlayback.exe plays the frames
# through the groom scene index the way usdview does without --allow-async,
# prints a per-frame table (with -Pull, a hash of the published points to
# compare builds) plus, per cook, why it compiled, which operators re-captured
# or reused their capture, how many chunks they evaluated (TF_DEBUG
# USDGEN_COMMIT / USDGEN_SCHEDULE / USDGEN_INGRESS) and, per frame, the dirty
# locators sent to Hydra; it writes
# trace.json (open in chrome://tracing or https://ui.perfetto.dev) and
# report.txt (OpenUSD's aggregate trace report).
#
# -Usdview: runs testusdview with tools\usdview_playback_trace.py, which steps
# the frames and repaints the viewport after each, with --traceToFile; the
# trace then includes Storm's sync and draw.

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

$scenePath = (Resolve-Path $Scene).Path
if (-not $OutDir) {
    $OutDir = Join-Path $Build ("traces\" + [IO.Path]::GetFileNameWithoutExtension($scenePath))
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

if (-not $Usdview) {
    $exe = Join-Path $Build "usdGenTracePlayback.exe"
    if (-not (Test-Path $exe)) { throw "$exe is missing. Build the usdGenTracePlayback target." }
    $arguments = @($scenePath, "--loops", $Loops,
                   "--chrome", (Join-Path $OutDir "trace.json"),
                   "--report", (Join-Path $OutDir "report.txt"))
    if ($Frames) { $arguments += @("--frames", $Frames) }
    if ($Pull) { $arguments += "--pull" }
    if ($Quiet) { $arguments += "--quiet" }
    # Windows PowerShell's Tee-Object writes UTF-16 and turns native stderr
    # into terminating errors; collect plain lines and write UTF-8 instead.
    $ErrorActionPreference = "Continue"
    $lines = & $exe @arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $lines | Set-Content -Encoding utf8 (Join-Path $OutDir "playback.log")
    $lines
    exit $code
}

$pythonDirs = @(
    (Join-Path $Build "python"),
    (Join-Path $UsdInstallDir "Lib\site-packages"),
    (Join-Path $UsdInstallDir "lib\python")
) | Where-Object { Test-Path $_ }
$env:PYTHONPATH = ($pythonDirs -join ';') + $(if ($env:PYTHONPATH) { ';' + $env:PYTHONPATH } else { '' })
$python = $env:PY
if (-not $python) {
    $python = Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
}
if (-not $python) { throw "Python is not on PATH. Set PY to the interpreter that imports this build's pxr." }
$testusdview = Join-Path $UsdInstallDir "bin\testusdview"
if (-not (Test-Path $testusdview)) { throw "testusdview is not installed in '$UsdInstallDir'." }

$env:USDGEN_TRACE_FRAMES = $Frames
$env:USDGEN_TRACE_LOOPS = "$Loops"
$env:USDGEN_TRACE_COMPLEXITY = $Complexity
$env:USDGEN_TRACE_TABLE = (Join-Path $OutDir "usdview_frames.txt")
if (-not $Quiet) { $env:TF_DEBUG = "USDGEN_COMMIT USDGEN_SCHEDULE USDGEN_INGRESS" }
$script = Join-Path $Root "tools\usdview_playback_trace.py"
$ErrorActionPreference = "Continue"
$traceFile = Join-Path $OutDir $(if ($TraceFormat -eq "chrome") { "usdview_trace.json" } else { "usdview_trace.txt" })
$lines = & $python $testusdview --testScript $script $scenePath `
    --traceToFile $traceFile --traceFormat $TraceFormat 2>&1 |
    ForEach-Object { "$_" }
$code = $LASTEXITCODE
$lines | Set-Content -Encoding utf8 (Join-Path $OutDir "usdview_playback.log")
$lines
exit $code
