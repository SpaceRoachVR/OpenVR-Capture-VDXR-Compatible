<#
.SYNOPSIS
    Installs and registers the SpaceRoachVR OpenXR / VDXR Capture Layer.

.DESCRIPTION
    Registers the OpenXR API Layer manifest in the Windows Registry under
    HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit so that VDXR and OpenXR
    applications automatically load the capture layer.
#>

[CmdletBinding()]
param(
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " SpaceRoachVR OpenXR / VDXR Capture Layer Installer" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# Determine script root
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RootDir = Split-Path -Parent $ScriptDir

if ([string]::IsNullOrEmpty($ManifestPath)) {
    # Check default locations
    $PossiblePaths = @(
        "$ScriptDir\openxr-vrcapture-layer.json",
        "$RootDir\openxr-vrcapture-layer.json",
        "$RootDir\build\layers\openxr-vrcapture-layer.json",
        "$RootDir\build\layers\openxr-vrcapture-layer\openxr-vrcapture-layer.json",
        "$RootDir\build_x64\layers\openxr-vrcapture-layer\Release\openxr-vrcapture-layer.json",
        "$RootDir\layers\openxr-vrcapture-layer\openxr-vrcapture-layer.json"
    )

    foreach ($p in $PossiblePaths) {
        if (Test-Path $p) {
            $ManifestPath = (Resolve-Path $p).Path
            break
        }
    }
}

if ([string]::IsNullOrEmpty($ManifestPath) -or (-not (Test-Path $ManifestPath))) {
    Write-Warning "Layer manifest JSON not found. Generating a local manifest..."
    $DllPath = "$RootDir\openxr-vrcapture-layer.dll"
    if (-not (Test-Path $DllPath)) {
        $DllPath = "$RootDir\build\layers\Release\openxr-vrcapture-layer.dll"
    }
    if (-not (Test-Path $DllPath)) {
        $DllPath = "$ScriptDir\openxr-vrcapture-layer.dll"
    }

    $ManifestPath = "$ScriptDir\openxr-vrcapture-layer.json"
    $ManifestContent = @"
{
    "file_format_version": "1.0.0",
    "api_layer": {
        "name": "XR_APILAYER_SPACEROACH_vr_capture",
        "type": "global",
        "library_path": "$($DllPath.Replace('\', '\\'))",
        "api_version": "1.0",
        "implementation_version": "1",
        "description": "SpaceRoachVR OpenXR & VDXR Capture Layer for OBS Studio",
        "functions": {
            "xrNegotiateLoaderApiLayerInterface": "xrNegotiateLoaderApiLayerInterface"
        }
    }
}
"@
    Set-Content -Path $ManifestPath -Value $ManifestContent -Encoding UTF8
    Write-Host "Created manifest at: $ManifestPath" -ForegroundColor Green
}

$RegistryPath = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"

if (-not (Test-Path $RegistryPath)) {
    New-Item -Path $RegistryPath -Force | Out-Null
}

New-ItemProperty -Path $RegistryPath -Name $ManifestPath -Value 0 -PropertyType DWord -Force | Out-Null

Write-Host "Successfully registered OpenXR API Layer in registry!" -ForegroundColor Green
Write-Host "Registry Key: $RegistryPath" -ForegroundColor Gray
Write-Host "Manifest:     $ManifestPath" -ForegroundColor Gray
Write-Host "`nVDXR and OpenXR titles will now automatically feed the OBS VR Capture plugin." -ForegroundColor Cyan
