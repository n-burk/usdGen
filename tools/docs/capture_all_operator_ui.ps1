# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
# Capture each runtime/schema operator in the real localized Noodles Editor.
param([string[]] $Operators = @())

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$catalog = Get-Content (Join-Path $repo 'docs\reference\operators.json') -Raw | ConvertFrom-Json
$media = Get-Content (Join-Path $repo 'docs\reference\media.json') -Raw | ConvertFrom-Json
$env:USDGEN_UI_CAPTURE_DIR = 'docs/site/media/ui'
$originalPath = $env:PATH
$originalPythonPath = $env:PYTHONPATH
$originalPluginPath = $env:PXR_PLUGINPATH_NAME

foreach ($record in $catalog.operators) {
    $id = $record.id
    if ($id -in @('UsdGenWidthBlend', 'UsdGenInstance')) { continue }
    if ($Operators.Count -gt 0 -and $id -notin $Operators) { continue }
    $scene = $media.operators.$id.scene
    if ($id -eq 'UsdGenPart') { $scene = 'examples/docs/operators/part.usda' }
    if ($id -eq 'UsdGenFreeze') { $scene = 'examples/docs/operators/freeze.usda' }
    if (-not $scene -or -not (Test-Path (Join-Path $repo $scene))) {
        throw "No operator UI scene for $id"
    }
    $env:USDGEN_UI_OPERATOR_ID = $id
    Write-Host "Capturing $id in $scene"
    try {
        & (Join-Path $repo 'bin\launch_usdview.ps1') -TestScript 'tools/docs/capture_operator_noodles_ui.py' $scene
        if ($LASTEXITCODE -ne 0) { throw "Noodles capture failed for $id ($LASTEXITCODE)" }
    } finally {
        $env:PATH = $originalPath
        $env:PYTHONPATH = $originalPythonPath
        $env:PXR_PLUGINPATH_NAME = $originalPluginPath
    }
}
