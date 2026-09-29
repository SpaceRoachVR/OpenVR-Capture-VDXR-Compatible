<#
.SYNOPSIS
    Installs and registers the SpaceRoachVR OpenXR / VDXR Capture Layer.

.DESCRIPTION
    Copies the layer DLL and its manifest to
        %ProgramFiles%\SpaceRoachVR\OpenXR Capture Layer
    and registers it as an implicit OpenXR API layer under
        HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit
    so every OpenXR game can load it. Machine-wide (HKLM) registration is the
    default because some launch paths only load HKLM layers -- notably Virtual
    Desktop's injection path for games using Meta's OVRPlugin, where layers
    registered under HKCU are never loaded. This needs administrator rights;
    the script asks for them (UAC) when needed.

    -CurrentUser registers under HKCU instead (no admin, but not seen by every
    game), pointing at the build output in place.

    Any previous registration of this layer, in either hive, is removed first
    so the loader never sees two copies of it.

    Close all OpenXR games before running: a loaded layer DLL can't be replaced.
#>

[CmdletBinding()]
param(
    [string]$ManifestPath = "",
    [switch]$CurrentUser
)

$ErrorActionPreference = "Stop"

$LayerName = "XR_APILAYER_SPACEROACH_vr_capture"
$ManifestFileName = "openxr-vrcapture-layer.json"
$DllFileName = "openxr-vrcapture-layer.dll"
$MachineKey = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$UserKey = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$InstallDir = Join-Path $env:ProgramFiles "SpaceRoachVR\OpenXR Capture Layer"

$ScriptPath = $MyInvocation.MyCommand.Definition
$ScriptDir = Split-Path -Parent $ScriptPath
$RootDir = Split-Path -Parent $ScriptDir

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $CurrentUser -and -not $IsAdmin) {
    Write-Host "Machine-wide install needs administrator rights; requesting elevation..." -ForegroundColor Yellow
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"")
    if ($ManifestPath) { $argList += @("-ManifestPath", "`"$ManifestPath`"") }
    $proc = Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs -Wait -PassThru
    exit $proc.ExitCode
}

Write-Host "======================================================" -ForegroundColor Cyan
Write-Host " SpaceRoachVR OpenXR / VDXR Capture Layer Installer" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

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

try {
    # ---- Find the build to install
    if (-not [string]::IsNullOrEmpty($ManifestPath)) {
        if (-not (Test-Path $ManifestPath) -or -not (Get-ManifestDll $ManifestPath)) {
            throw "Manifest '$ManifestPath' not found, or the DLL it points to does not exist."
        }
        $ManifestPath = (Resolve-Path $ManifestPath).Path
    } else {
        # Release package layout first, then build output folders. Of the
        # usable manifests, take the one whose DLL was built most recently,
        # so a stale copy (e.g. an old bin\ folder) never wins.
        $Candidates = @(
            "$ScriptDir\$ManifestFileName",
            "$RootDir\$ManifestFileName",
            "$RootDir\openxr-layer\$ManifestFileName",
            "$RootDir\bin\$ManifestFileName",
            "$RootDir\build\layers\Release\$ManifestFileName",
            "$RootDir\build\layers\RelWithDebInfo\$ManifestFileName",
            "$RootDir\build\ninja-layer\$ManifestFileName",
            "$RootDir\build\ci-layers\Release\$ManifestFileName"
        )
        $ManifestPath = $Candidates |
            Where-Object { (Test-Path $_) -and (Get-ManifestDll $_) } |
            Sort-Object { (Get-Item (Get-ManifestDll $_)).LastWriteTime } -Descending |
            Select-Object -First 1
        if (-not $ManifestPath) {
            throw "Could not find a built layer ($ManifestFileName next to $DllFileName). Build it first (see README) or pass -ManifestPath."
        }
        $ManifestPath = (Resolve-Path $ManifestPath).Path
    }
    $SourceDll = Get-ManifestDll $ManifestPath
    Write-Host "Layer build: $SourceDll" -ForegroundColor Gray

    if ($CurrentUser) {
        $RegisteredManifest = $ManifestPath
        $TargetKey = $UserKey
    } else {
        # ---- Copy into Program Files so games never lock the build output.
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        try {
            Copy-Item $SourceDll (Join-Path $InstallDir $DllFileName) -Force
        } catch {
            throw "Could not replace the installed layer DLL - close all OpenXR/VR games and try again. ($($_.Exception.Message))"
        }
        # Keep this in sync with layers/openxr-vrcapture-layer/layer_manifest.json.in.
        # disable_environment is REQUIRED for implicit layers.
        $Manifest = [ordered]@{
            file_format_version = "1.0.0"
            api_layer = [ordered]@{
                name = $LayerName
                library_path = ".\$DllFileName"
                api_version = "1.0"
                implementation_version = "1"
                description = "SpaceRoachVR OpenXR & VDXR Capture Layer for OBS Studio"
                disable_environment = "DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE"
                functions = [ordered]@{
                    xrNegotiateLoaderApiLayerInterface = "xrNegotiateLoaderApiLayerInterface"
                }
            }
        }
        $RegisteredManifest = Join-Path $InstallDir $ManifestFileName
        # UTF-8 without BOM: some JSON parsers reject a BOM.
        [System.IO.File]::WriteAllText($RegisteredManifest, ($Manifest | ConvertTo-Json -Depth 5),
            (New-Object System.Text.UTF8Encoding $false))
        $TargetKey = $MachineKey
        Write-Host "Installed to: $InstallDir" -ForegroundColor Green
    }

    # ---- Drop every other registration of this layer, in both hives.
    foreach ($key in @($UserKey, $MachineKey)) {
        if (-not (Test-Path $key)) { continue }
        if ($key -eq $MachineKey -and -not $IsAdmin) { continue }
        $props = Get-ItemProperty -Path $key
        foreach ($prop in $props.PSObject.Properties) {
            if ($prop.Name -like "*$ManifestFileName" -and -not ($key -eq $TargetKey -and $prop.Name -eq $RegisteredManifest)) {
                Remove-ItemProperty -Path $key -Name $prop.Name -Force
                Write-Host "Removed old registration: $($key.Split(':')[0]) $($prop.Name)" -ForegroundColor Yellow
            }
        }
    }

    if (-not (Test-Path $TargetKey)) {
        New-Item -Path $TargetKey -Force | Out-Null
    }
    # Value 0 = enabled, non-zero = disabled.
    New-ItemProperty -Path $TargetKey -Name $RegisteredManifest -Value 0 -PropertyType DWord -Force | Out-Null

    Write-Host "Registered OpenXR API layer ($(if ($CurrentUser) { 'current user' } else { 'all users' }))" -ForegroundColor Green
    Write-Host "Registry Key: $TargetKey" -ForegroundColor Gray
    Write-Host "Manifest:     $RegisteredManifest" -ForegroundColor Gray
    Write-Host "`nOpenXR / VDXR games will now feed the OBS VR Capture plugin." -ForegroundColor Cyan
    Write-Host "To bypass the layer for a single app, set DISABLE_XR_APILAYER_SPACEROACH_VR_CAPTURE=1." -ForegroundColor Gray
    Write-Host "Diagnostic log: %LOCALAPPDATA%\SpaceRoachVR\openxr-vrcapture-layer.log" -ForegroundColor Gray
    $exitCode = 0
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}

# When we were started elevated in a separate window, keep it open to read.
if (-not $CurrentUser -and $IsAdmin -and $Host.Name -eq "ConsoleHost" -and [Environment]::GetCommandLineArgs() -contains "-File") {
    Read-Host "Press Enter to close"
}
exit $exitCode
