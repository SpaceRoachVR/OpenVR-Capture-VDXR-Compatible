<#
.SYNOPSIS
    Installs a locally built win-openvr.dll into OBS's per-machine plugin folder.

.DESCRIPTION
    Copies the plugin and its locale into
        %ProgramData%\obs-studio\plugins\win-openvr\bin\64bit\
        %ProgramData%\obs-studio\plugins\win-openvr\data\
    which OBS 28+ loads without admin rights. The currently installed
    win-openvr.dll is backed up once as win-openvr.dll.pre-install (an existing
    backup is never overwritten). An existing openvr_api.dll is kept; if none is
    present, one is copied from SteamVR.

    OBS must be closed.
#>

[CmdletBinding()]
param(
    [string]$PluginDll = ""
)

$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
$PluginDir = Join-Path $env:ProgramData "obs-studio\plugins\win-openvr"
$BinDir = Join-Path $PluginDir "bin\64bit"
$DataDir = Join-Path $PluginDir "data"

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close it first, then run this script again."
}

if ([string]::IsNullOrEmpty($PluginDll)) {
    $PluginDll = @(
        "$RootDir\build\ninja-plugin\win-openvr.dll",
        "$RootDir\build\plugin\Release\win-openvr.dll",
        "$RootDir\build\ci-plugin\Release\win-openvr.dll"
    ) | Where-Object { Test-Path $_ } | Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
}
if (-not $PluginDll -or -not (Test-Path $PluginDll)) {
    throw "No built win-openvr.dll found. Build plugins/win-openvr first (see README) or pass -PluginDll."
}

New-Item -ItemType Directory -Force -Path $BinDir, (Join-Path $DataDir "locale") | Out-Null

$Installed = Join-Path $BinDir "win-openvr.dll"
$Backup = Join-Path $BinDir "win-openvr.dll.pre-install"
if ((Test-Path $Installed) -and -not (Test-Path $Backup)) {
    Copy-Item $Installed $Backup
    Write-Host "Backed up previous plugin to $Backup" -ForegroundColor Yellow
}

Copy-Item $PluginDll $Installed -Force
Copy-Item "$RootDir\plugins\win-openvr\data\locale\*" (Join-Path $DataDir "locale") -Force

$OpenVrApi = Join-Path $BinDir "openvr_api.dll"
if (-not (Test-Path $OpenVrApi)) {
    $steam = (Get-ItemProperty HKCU:\Software\Valve\Steam -ErrorAction SilentlyContinue).SteamPath
    $candidate = if ($steam) { Join-Path $steam "steamapps\common\SteamVR\bin\win64\openvr_api.dll" }
    if ($candidate -and (Test-Path $candidate)) {
        Copy-Item $candidate $OpenVrApi
        Write-Host "Copied openvr_api.dll from SteamVR" -ForegroundColor Yellow
    } else {
        Write-Warning "openvr_api.dll not found: the plugin won't load until it is placed in $BinDir"
    }
}

Write-Host "Installed $PluginDll" -ForegroundColor Green
Write-Host "  -> $Installed" -ForegroundColor Gray
Write-Host "To roll back: copy win-openvr.dll.pre-install over win-openvr.dll" -ForegroundColor Gray
