# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('clumped', 'unclumped', 'both')]
    [string]$Variant,
    [Parameter(Mandatory = $true)]
    [string]$Baker,
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDir,
    [switch]$PinViewerRuntime
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$bakerPath = (Resolve-Path -LiteralPath $Baker).Path
$runtimePath = (Resolve-Path -LiteralPath $RuntimeDir).Path
$python = (Get-Command python -ErrorAction Stop).Source
$scratch = Join-Path $repo 'renders\docs\wind-variants'
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

# A stopped parent can leave this file behind. Match the command line as well
# as the PID so a reused Windows PID never blocks an unrelated process.
Get-ChildItem -LiteralPath $scratch -Filter 'wind-*-launch.json' | ForEach-Object {
    $entry = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$($entry.pid)"
    if ($process -and $process.CommandLine -like '*render_wind_variants.py*') {
        throw "Wind variant render already running: PID $($entry.pid) ($($entry.variant))"
    }
}

$requested = if ($Variant -eq 'both') { @('clumped', 'unclumped') } else { @($Variant) }
$completeCount = 0
foreach ($member in $requested) {
    $candidate = Join-Path $scratch "wind-$member-state.json"
    if (Test-Path -LiteralPath $candidate) {
        $state = Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json
        if ($state.status -eq 'complete') { $completeCount++ }
    }
}
if ($completeCount -eq $requested.Count) {
    throw "All requested Wind variants are already complete; inspect their checkpoints before a rerun"
}

$helper = Join-Path $repo 'tools\launch_wind_breakaway.py'
$arguments = @($helper, '--variant', $Variant, '--baker', $bakerPath,
               '--runtime-dir', $runtimePath)
if ($PinViewerRuntime) { $arguments += '--pin-viewer-runtime' }
& $python @arguments
if ($LASTEXITCODE -ne 0) { throw "Wind breakaway launcher failed ($LASTEXITCODE)" }
