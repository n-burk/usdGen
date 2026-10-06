# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
# Launches Houdini with the usdGen USD plugins built in build-houdini.
#
# Usage: bin\launch_houdini_usdgen.ps1 [-Mode gui|hbatch|hython|check] [-HoudiniRoot <path>] [-Install <path>] [-App <binary>] [extra args...]
#
#   gui     Start the Houdini GUI (default). Extra args are passed through.
#   hbatch  Start hbatch (batch Houdini) with the usdGen environment.
#   hython  Start hython (Houdini's Python) with the usdGen environment.
#   check   Run the headless plugin verification (hython
#           bin\check_houdini_usdgen.py) and report. No GUI.
#
# The Houdini root comes from -HoudiniRoot, HOUDINI_ROOT, or HFS (in that
# order); the usdGen install from -Install (default: build-houdini\install
# beside this script); the GUI binary from -App (default:
# hindie.steam.exe).
param(
    [ValidateSet("gui", "hbatch", "hython", "check")]
    [string] $Mode = "gui",
    [string] $HoudiniRoot = "",
    [string] $Install = "",
    [string] $App = "hindie.steam.exe",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest = @()
)

# Continue, not Stop: hython/hbatch print breakpad/Steam notices on stderr,
# which PowerShell would otherwise turn into terminating errors. Failures
# surface through explicit throws above and $LASTEXITCODE below.
$ErrorActionPreference = "Continue"
$Gen = (Resolve-Path (Join-Path $PSScriptRoot ".."))
if (-not $HoudiniRoot) { $HoudiniRoot = $env:HOUDINI_ROOT }
if (-not $HoudiniRoot) { $HoudiniRoot = $env:HFS }
if (-not $HoudiniRoot) {
    throw "No Houdini root: pass -HoudiniRoot or set HOUDINI_ROOT (or HFS)."
}
if (-not (Test-Path (Join-Path $HoudiniRoot "bin\hython.exe"))) {
    throw "No Houdini install at '$HoudiniRoot'."
}
if (-not $Install) { $Install = Join-Path $Gen "build-houdini\install" }
$schemaResources = Join-Path $Install "lib\usd\usdGenSchema\resources\plugInfo.json"
if (-not (Test-Path $schemaResources)) {
    throw "No usdGen install at '$Install'. Run bin\build_usdgen_houdini.ps1 first (or pass -Install with the install prefix)."
}
if (-not (Test-Path (Join-Path $Install "lib\usdGenImaging.dll"))) {
    throw "'$Install\lib\usdGenImaging.dll' is missing. Re-run bin\build_usdgen_houdini.ps1."
}

# Scrub inherited entries that would shadow Houdini's embedded Python
# 3.13 or its vendored USD before composing the child environment: a
# directory shipping python3*.dll (CPython 3.10's python310.dll sits on a
# default PATH), a foreign pxr package (the stock build's cp310 binaries
# fail under 3.13 with "DLL load failed while importing _tf"), a
# stock-build DLL dir whose file names collide with the Houdini twins, or
# anything MoonRay. Our own directories are prepended after the scrub, so
# they are unaffected; drops are reported to stderr. PYTHONHOME must be
# empty or the embedded interpreter initializes against the wrong stdlib.
function Invoke-ScrubEnv([string] $Name) {
    $current = (Get-Item "Env:$Name" -ErrorAction SilentlyContinue).Value
    if ($null -eq $current) { return }
    $kept = @()
    foreach ($seg in ($current -split ';')) {
        $seg = "$seg".Trim().Trim('"')
        if (-not $seg) { continue }
        $hostile = (Test-Path (Join-Path $seg 'python3*.dll')) -or
            (Test-Path (Join-Path $seg 'pxr\__init__.py')) -or
            (Test-Path (Join-Path $seg 'usd_usd.dll')) -or
            (Test-Path (Join-Path $seg 'rigExec.dll')) -or
            (Test-Path (Join-Path $seg 'usdGen*.dll')) -or
            (Test-Path (Join-Path $seg 'usdMayaRig.dll')) -or
            (Test-Path (Join-Path $seg 'hdMoonray*.dll')) -or
            ("$seg" -match 'moonray')
        if ($hostile) {
            [Console]::Error.WriteLine("[launch_houdini_usdgen] dropped hostile ${Name} segment: $seg")
        } else {
            $kept += $seg
        }
    }
    Set-Item "Env:$Name" ($kept -join ';')
}
Invoke-ScrubEnv 'PATH'
Invoke-ScrubEnv 'PYTHONPATH'
Invoke-ScrubEnv 'PXR_PLUGINPATH_NAME'
Remove-Item Env:\PYTHONHOME -ErrorAction SilentlyContinue
Remove-Item Env:\PYTHONEXECUTABLE -ErrorAction SilentlyContinue

# The usdGen DLLs live in install\lib (plain directory, not a Houdini dso
# path), so they ride on PATH. The USD plugins register through
# HOUDINI_USD_DSO_PATH: Houdini assembles Plug's search list itself and
# does not honor PXR_PLUGINPATH_NAME, which is still set for any stock-USD
# tool pointed at this install. The trailing ;& keeps Houdini's own
# entries. The usdview-only python plugins (usdGenTools, usdGenPomadeTools)
# are deliberately left out.
$usdDir = Join-Path $Install "lib\usd"
$env:PATH = "$(Join-Path $Install 'lib');$(Join-Path $HoudiniRoot 'bin');$env:PATH"
$env:PXR_PLUGINPATH_NAME = "$usdDir;$env:PXR_PLUGINPATH_NAME"
$pluginDirs = @("usdGenSchema", "usdGenImaging", "usdGenPomade", "usdGenShaders") |
    ForEach-Object { Join-Path $usdDir "$_\resources" }
$env:HOUDINI_USD_DSO_PATH = "$($pluginDirs -join ';');&"

function Invoke-Houdini([string] $Exe, [string[]] $Arguments) {
    # Stringify the child's stderr as it streams: hython/hbatch print
    # breakpad/Steam notices there, which PowerShell would otherwise
    # render as red NativeCommandError essays.
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    exit $LASTEXITCODE
}

switch ($Mode) {
    "gui" {
        $exe = Join-Path $HoudiniRoot "bin\$App"
        if (-not (Test-Path $exe)) {
            throw "'$exe' not found. Pass -App with another entry point."
        }
        # A direct launch outside the Steam client still needs the Steam
        # app id in the environment, or the GUI exits immediately with
        # code 3. An explicit SteamAppId wins; otherwise it is read from
        # the install.
        if (-not $env:SteamAppId) {
            $appid = Join-Path $HoudiniRoot "steam_appid.txt"
            if (Test-Path $appid) {
                $env:SteamAppId = (Get-Content $appid -Raw).Trim()
            }
        }
        if (-not $env:SteamGameId -and $env:SteamAppId) {
            $env:SteamGameId = $env:SteamAppId
        }
        Start-Process $exe -ArgumentList $Rest
    }
    "hbatch" { Invoke-Houdini (Join-Path $HoudiniRoot "bin\hbatch.exe") $Rest }
    "hython" { Invoke-Houdini (Join-Path $HoudiniRoot "bin\hython.exe") $Rest }
    "check" {
        Invoke-Houdini (Join-Path $HoudiniRoot "bin\hython.exe") `
            (@((Join-Path $Gen "bin\check_houdini_usdgen.py")) + $Rest)
    }
}
