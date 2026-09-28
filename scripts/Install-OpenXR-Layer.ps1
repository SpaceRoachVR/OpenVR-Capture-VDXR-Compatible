<#
.SYNOPSIS
    Installs and registers the SpaceRoachVR OpenXR / VDXR Capture Layer.

.DESCRIPTION
    Registers the OpenXR API Layer manifest in the Windows Registry under
    HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit so that VDXR and OpenXR
    applications automatically load the capture layer.

    Any previous registration of this layer (e.g. from an older build folder)
    is removed first, so the loader never sees two copies of the layer.
#>

[CmdletBinding()]
param(
    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"

$LayerName = "XR_APILAYER_SPACEROACH_vr_capture"
$ManifestFileName = "openxr-vrcapture-layer.json"
$DllFileName = "openxr-vrcapture-layer.dll"
$RegistryPath = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " SpaceRoachVR OpenXR / VDXR Capture Layer Installer" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RootDir = Split-Path -Parent $ScriptDir

# Returns the manifest's resolved DLL path if the manifest is usable, else $null.
function Get-ManifestDll([string]$Path) {
    try {
        $json = Get-Content -Raw -Path $Path | ConvertFrom-Json
    } catch {
        return $null
    }
    $lib = $json.api_layer.library_path
    if ([string]::IsNullOrEmpty($lib)) { return $null }
    if (-not [System.IO.Path]::IsPathRooted($lib)) {
        $lib = Join-Path (Split-Path -Parent $Path) $lib
    }
    if (Test-Path $lib) { return (Resolve-Path $lib).Path }
    return $null
}

if (-not [string]::IsNullOrEmpty($ManifestPath)) {
    if (-not (Test-Path $ManifestPath) -or -not (Get-ManifestDll $ManifestPath)) {
        throw "Manifest '$ManifestPath' not found, or the DLL it points to does not exist."
    }
    $ManifestPath = (Resolve-Path $ManifestPath).Path
} else {
    # Release package layout first, then common build output folders. The
    # build writes the manifest next to the DLL, so each candidate is only
    # accepted if the DLL it references actually exists.
    $Candidates = @(
        "$ScriptDir\$ManifestFileName",
        "$RootDir\$ManifestFileName",
        "$RootDir\bin\$ManifestFileName",
        "$RootDir\build\layers\Release\$ManifestFileName",
        "$RootDir\build\layers\RelWithDebInfo\$ManifestFileName",
        "$RootDir\build\layers\$ManifestFileName",
        "$RootDir\build\ninja-layer\$ManifestFileName"
    )
    # Of the usable manifests, take the one whose DLL was built most recently,
    # so a stale copy (e.g. an old bin\ folder) never wins over a fresh build.
    $ManifestPath = $Candidates |
        Where-Object { (Test-Path $_) -and (Get-ManifestDll $_) } |
        Sort-Object { (Get-Item (Get-ManifestDll $_)).LastWriteTime } -Descending |
        Select-Object -First 1
    if ($ManifestPath) {
        $ManifestPath = (Resolve-Path $ManifestPath).Path
    }
}

if ([string]::IsNullOrEmpty($ManifestPath)) {
    Write-Warning "No usable layer manifest found. Looking for the layer DLL to generate one..."
    $DllCandidates = @(
        "$ScriptDir\$DllFileName",
        "$RootDir\$DllFileName",
        "$RootDir\bin\$DllFileName",
        "$RootDir\build\layers\Release\$DllFileName",
        "$RootDir\build\ninja-layer\$DllFileName"
    )
    $DllPath = $DllCandidates | Where-Object { Test-Path $_ } |
        Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
    if (-not $DllPath) {
        throw "Could not find $DllFileName. Build the layer first (see README) or pass -ManifestPath."
    }
    $DllPath = (Resolve-Path $DllPath).Path

    # Keep this in sync with layers/openxr-vrcapture-layer/layer_manifest.json.in.
    # disable_environment is REQUIRED for implicit layers: the OpenXR loader
    # silently ignores implicit layer manifests that omit it.
    $Manifest = [ordered]@{
        file_format_version = "1.0.0"
        api_layer = [ordered]@{
            name = $LayerName
            type = "global"
            library_path = $DllPath
            api_version = "1.0"
            implementation_version = "1"
            description = "SpaceRoachVR OpenXR & VDXR Capture Layer for OBS Studio"
            disable_environment = "DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE"
            functions = [ordered]@{
                xrNegotiateLoaderApiLayerInterface = "xrNegotiateLoaderApiLayerInterface"
            }
        }
    }
    $ManifestPath = Join-Path (Split-Path -Parent $DllPath) $ManifestFileName
    # UTF-8 without BOM: some JSON parsers reject a BOM.
    [System.IO.File]::WriteAllText($ManifestPath, ($Manifest | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Created manifest at: $ManifestPath" -ForegroundColor Green
}

if (-not (Test-Path $RegistryPath)) {
    New-Item -Path $RegistryPath -Force | Out-Null
}

# Drop stale registrations of this layer (other folders, older builds).
$Props = Get-ItemProperty -Path $RegistryPath
foreach ($Prop in $Props.PSObject.Properties) {
    if ($Prop.Name -like "*$ManifestFileName" -and $Prop.Name -ne $ManifestPath) {
        Remove-ItemProperty -Path $RegistryPath -Name $Prop.Name -Force
        Write-Host "Removed stale registration: $($Prop.Name)" -ForegroundColor Yellow
    }
}

# Value 0 = enabled, non-zero = disabled.
New-ItemProperty -Path $RegistryPath -Name $ManifestPath -Value 0 -PropertyType DWord -Force | Out-Null

Write-Host "Successfully registered OpenXR API Layer in registry!" -ForegroundColor Green
Write-Host "Registry Key: $RegistryPath" -ForegroundColor Gray
Write-Host "Manifest:     $ManifestPath" -ForegroundColor Gray
Write-Host "DLL:          $(Get-ManifestDll $ManifestPath)" -ForegroundColor Gray
Write-Host "`nVDXR and OpenXR titles will now automatically feed the OBS VR Capture plugin." -ForegroundColor Cyan
Write-Host "To bypass the layer for a single app, set DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE=1." -ForegroundColor Gray
