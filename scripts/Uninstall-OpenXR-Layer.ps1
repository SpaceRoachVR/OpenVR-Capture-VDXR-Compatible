<#
.SYNOPSIS
    Uninstalls and unregisters the SpaceRoachVR OpenXR / VDXR Capture Layer.

.DESCRIPTION
    Removes the OpenXR API Layer manifest entries from the Windows Registry.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "SilentlyContinue"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " SpaceRoachVR OpenXR / VDXR Capture Layer Uninstaller" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

$RegistryPath = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"

if (Test-Path $RegistryPath) {
    $Props = Get-ItemProperty -Path $RegistryPath
    foreach ($Prop in $Props.PSObject.Properties) {
        if ($Prop.Name -like "*openxr-vrcapture-layer.json*") {
            Remove-ItemProperty -Path $RegistryPath -Name $Prop.Name -Force
            Write-Host "Removed registry entry: $($Prop.Name)" -ForegroundColor Green
        }
    }
}

Write-Host "OpenXR Capture Layer unregistered successfully." -ForegroundColor Cyan
