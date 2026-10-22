param(
    [Parameter(Mandatory = $true)]
    [string] $SdkRoot
)

$ErrorActionPreference = "Stop"

$sdkPath = [System.IO.Path]::GetFullPath($SdkRoot)
$packageRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$coreRoot = Join-Path $sdkPath "Core"
$includeSource = Join-Path $coreRoot "include"
$libraryRoot = Join-Path $coreRoot "lib\windows\x86_64"
$runtimeRoot = Join-Path $coreRoot "dll\windows\x86_64"
$headerSource = Join-Path $includeSource "Live2DCubismCore.h"
$manifestPath = Join-Path $packageRoot "package.json"

if (-not (Test-Path -LiteralPath $headerSource -PathType Leaf)) {
    throw "Live2DCubismCore.h was not found under '$includeSource'."
}

$headerText = Get-Content -LiteralPath $headerSource -Raw
$renderOrderApi = if ($headerText -match "csmGetRenderOrders\s*\(") {
    if ($headerText -notmatch "csmGetOffscreenCount\s*\(") {
        throw "The Cubism 5.3 offscreen API is missing from the selected Core header."
    }
    "csmGetRenderOrders"
} elseif ($headerText -match "csmGetDrawableRenderOrders\s*\(") {
    "csmGetDrawableRenderOrders"
} else {
    throw "This Cubism Core header has no supported render-order API."
}

$librarySource = Get-ChildItem -LiteralPath $libraryRoot -Filter "Live2DCubismCore*.lib" -File -Recurse |
    Where-Object { $_.Name -match "_MD\.lib$" } |
    Select-Object -First 1
$runtimeSource = Get-ChildItem -LiteralPath $runtimeRoot -Filter "Live2DCubismCore.dll" -File -Recurse |
    Select-Object -First 1

if ($null -eq $librarySource) {
    throw "An MSVC /MD import library named Live2DCubismCore_MD.lib was not found under '$libraryRoot'."
}
if ($null -eq $runtimeSource) {
    throw "Live2DCubismCore.dll was not found under '$runtimeRoot'."
}

$runtimeStream = [System.IO.File]::OpenRead($runtimeSource.FullName)
try {
    $runtimeReader = [System.IO.BinaryReader]::new($runtimeStream)
    if ($runtimeReader.ReadUInt16() -ne 0x5A4D) {
        throw "Live2DCubismCore.dll has no valid PE header."
    }
    $runtimeStream.Position = 0x3C
    $peOffset = $runtimeReader.ReadInt32()
    if ($peOffset -lt 64) {
        throw "Live2DCubismCore.dll has an invalid PE header offset."
    }
    $runtimeStream.Position = $peOffset
    if ($runtimeReader.ReadUInt32() -ne 0x00004550) {
        throw "Live2DCubismCore.dll has no valid PE signature."
    }
    if ($runtimeReader.ReadUInt16() -ne 0x8664) {
        throw "Live2DCubismCore.dll is not a Windows x64 binary."
    }
} finally {
    if ($null -ne $runtimeReader) {
        $runtimeReader.Dispose()
    } else {
        $runtimeStream.Dispose()
    }
}

$sdkDestination = Join-Path $packageRoot "sdk\Core"
$includeDestination = Join-Path $sdkDestination "include"
$libraryDestination = Join-Path $sdkDestination "lib\windows\x86_64"
$runtimeDestination = Join-Path $sdkDestination "dll\windows\x86_64"
New-Item -ItemType Directory -Path $includeDestination -Force | Out-Null
New-Item -ItemType Directory -Path $libraryDestination -Force | Out-Null
New-Item -ItemType Directory -Path $runtimeDestination -Force | Out-Null
Get-ChildItem -LiteralPath $includeSource -Force |
    Copy-Item -Destination $includeDestination -Recurse -Force
Copy-Item -LiteralPath $librarySource.FullName -Destination (Join-Path $libraryDestination "Live2DCubismCore_MD.lib") -Force
Copy-Item -LiteralPath $runtimeSource.FullName -Destination (Join-Path $runtimeDestination "Live2DCubismCore.dll") -Force

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$defines = @("LAMAPON_LIVE2D_CUBISM_ENABLED")
if ($renderOrderApi -eq "csmGetRenderOrders") {
    $defines += "LAMAPON_LIVE2D_CUBISM_53"
}
$native = $manifest.native
if ($null -eq $native) {
    $native = [ordered]@{}
}
$native.includeDirectories = @(
    @($native.includeDirectories) + "sdk/Core/include" |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
$native.libraries = @(
    @($native.libraries) + "sdk/Core/lib/windows/x86_64/Live2DCubismCore_MD.lib" |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
$native.runtimeFiles = @(
    @($native.runtimeFiles) + "sdk/Core/dll/windows/x86_64/Live2DCubismCore.dll" |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
$native.defines = @(
    @($native.defines) + $defines |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Where-Object { $_ -ne "LAMAPON_LIVE2D_CUBISM_53" -or $renderOrderApi -eq "csmGetRenderOrders" } |
        Sort-Object -Unique)
$manifest.native = $native
$manifestJson = $manifest | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Cubism Core was copied into '$sdkDestination'."
Write-Host "Restart LamaPon, then rebuild the project Game Module."
