<#
.SYNOPSIS
    Uninstalls and unregisters the SpaceRoachVR OpenXR / VDXR Capture Layer.

.DESCRIPTION
    Removes this layer's registrations from both
        HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit   (all users)
        HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit   (current user)
    and deletes %ProgramFiles%\SpaceRoachVR\OpenXR Capture Layer. Requests
    administrator rights (UAC) when a machine-wide install is present.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$ManifestFileName = "openxr-vrcapture-layer.json"
$MachineKey = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$UserKey = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$InstallDir = Join-Path $env:ProgramFiles "SpaceRoachVR\OpenXR Capture Layer"
$ScriptPath = $MyInvocation.MyCommand.Definition

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

function Get-Registrations([string]$Key) {
    if (-not (Test-Path $Key)) { return @() }
    (Get-ItemProperty -Path $Key).PSObject.Properties |
        Where-Object { $_.Name -like "*$ManifestFileName" } | ForEach-Object { $_.Name }
}

$needsMachine = (Get-Registrations $MachineKey).Count -gt 0 -or (Test-Path $InstallDir)
if ($needsMachine -and -not $IsAdmin) {
    Write-Host "Removing the machine-wide install needs administrator rights; requesting elevation..." -ForegroundColor Yellow
    $proc = Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"") -Verb RunAs -Wait -PassThru
    exit $proc.ExitCode
}

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " SpaceRoachVR OpenXR / VDXR Capture Layer Uninstaller" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

foreach ($key in @($UserKey, $MachineKey)) {
    foreach ($name in (Get-Registrations $key)) {
        Remove-ItemProperty -Path $key -Name $name -Force
        Write-Host "Removed registration: $($key.Split(':')[0]) $name" -ForegroundColor Green
    }
}

if (Test-Path $InstallDir) {
    try {
        Remove-Item -Recurse -Force $InstallDir
        Write-Host "Deleted $InstallDir" -ForegroundColor Green
    } catch {
        Write-Warning "Could not delete $InstallDir (is an OpenXR game still running?): $($_.Exception.Message)"
    }
}

Write-Host "OpenXR Capture Layer unregistered." -ForegroundColor Cyan
if ($IsAdmin -and [Environment]::GetCommandLineArgs() -contains "-File") {
    Read-Host "Press Enter to close"
}
