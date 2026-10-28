param(
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Destination -PathType Container)) {
    throw "Deployment directory does not exist: $Destination"
}

# Use the v142 redistributable, even if the runner also has a newer toolset.
$redistRoot = Join-Path $env:VCINSTALLDIR 'Redist\MSVC'
$runtime = Get-ChildItem $redistRoot -Directory -Filter '14.29.*' |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $runtime) { throw "MSVC v142 (14.29) redistributable not found in $redistRoot" }
$crt = Join-Path $runtime.FullName 'x64\Microsoft.VC142.CRT'
if (-not (Test-Path "$crt\msvcp140.dll")) { throw "Missing v142 CRT: $crt" }
Copy-Item "$crt\*.dll" $Destination -Force

# Server 2012 does not include UCRT by default. Deploy the Windows 10 SDK's
# downlevel UCRT beside the app instead of requiring a system installation.
$ucrt = Join-Path $env:WindowsSdkDir 'Redist\10.0.19041.0\ucrt\DLLs\x64'
if (-not (Test-Path "$ucrt\ucrtbase.dll")) { throw "Windows SDK 10.0.19041.0 UCRT not found: $ucrt" }
Copy-Item "$ucrt\*.dll" $Destination -Force

# Deploy software rasterizer and Direct3D compiler if available in Qt or Windows SDK
if ($env:QT_ROOT_DIR) {
    $qtOpenglsw = Join-Path $env:QT_ROOT_DIR 'bin\opengl32sw.dll'
    if (Test-Path $qtOpenglsw) {
        Copy-Item $qtOpenglsw $Destination -Force
    }
    $qtD3DCompiler = Join-Path $env:QT_ROOT_DIR 'bin\d3dcompiler_47.dll'
    if (Test-Path $qtD3DCompiler) {
        Copy-Item $qtD3DCompiler $Destination -Force
    }
}
if (-not (Test-Path "$Destination\d3dcompiler_47.dll") -and $env:WindowsSdkDir) {
    $sdkD3DCompiler = Join-Path $env:WindowsSdkDir 'Redist\D3D\x64\d3dcompiler_47.dll'
    if (Test-Path $sdkD3DCompiler) {
        Copy-Item $sdkD3DCompiler $Destination -Force
    }
}

# Deploy software rendering launch script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmdScript = Join-Path $scriptDir 'HarmonyCast-SoftwareRendering.cmd'
if (Test-Path $cmdScript) {
    Copy-Item $cmdScript $Destination -Force
}
