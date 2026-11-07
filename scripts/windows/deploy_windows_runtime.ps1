param(
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Destination -PathType Container)) {
    throw "Deployment directory does not exist: $Destination"
}

# 1. Locate and deploy MSVC CRT redistributable DLLs (msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll)
$redistRoot = ""
if ($env:VCINSTALLDIR -and (Test-Path (Join-Path $env:VCINSTALLDIR 'Redist\MSVC'))) {
    $redistRoot = Join-Path $env:VCINSTALLDIR 'Redist\MSVC'
} elseif ($env:VCToolsRedistDir -and (Test-Path $env:VCToolsRedistDir)) {
    $redistRoot = $env:VCToolsRedistDir
} else {
    $vsRoots = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Redist\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\MSVC",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Redist\MSVC"
    )
    foreach ($r in $vsRoots) {
        if (Test-Path $r) {
            $redistRoot = $r
            break
        }
    }
}

$crtFound = $false
if ($redistRoot) {
    # Recursively find x64 CRT directory under redist root regardless of subfolder naming scheme
    $crtDirs = Get-ChildItem $redistRoot -Directory -Recurse |
        Where-Object { $_.FullName -like "*x64*CRT*" } |
        Sort-Object LastWriteTime -Descending
    foreach ($d in $crtDirs) {
        Copy-Item "$($d.FullName)\*.dll" $Destination -Force
        Write-Host "Deployed CRT DLLs from $($d.FullName)"
        $crtFound = $true
        break
    }
}

# Fallback: Guarantee msvcp140.dll and vcruntime140.dll are present
$criticalCrt = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
foreach ($f in $criticalCrt) {
    if (-not (Test-Path (Join-Path $Destination $f))) {
        $sysCrt = Join-Path $env:windir "System32\$f"
        if (Test-Path $sysCrt) {
            Copy-Item $sysCrt $Destination -Force
            Write-Host "Fallback: Deployed $f from System32"
            $crtFound = $true
        }
    }
}

if (-not (Test-Path (Join-Path $Destination "vcruntime140.dll"))) {
    throw "Fatal: vcruntime140.dll could not be located for deployment. Aborting."
}

# 2. Deploy software rasterizer and Direct3D compiler from Qt or Windows SDK
if ($env:QT_ROOT_DIR) {
    $qtOpenglsw = Join-Path $env:QT_ROOT_DIR 'bin\opengl32sw.dll'
    if (Test-Path $qtOpenglsw) {
        Copy-Item $qtOpenglsw $Destination -Force
        Write-Host "Deployed opengl32sw.dll from Qt bin"
    }
    $qtD3DCompiler = Join-Path $env:QT_ROOT_DIR 'bin\d3dcompiler_47.dll'
    if (Test-Path $qtD3DCompiler) {
        Copy-Item $qtD3DCompiler $Destination -Force
        Write-Host "Deployed d3dcompiler_47.dll from Qt bin"
    }
}
if ((-not (Test-Path "$Destination\d3dcompiler_47.dll")) -and $env:WindowsSdkDir) {
    $sdkD3DCompiler = Join-Path $env:WindowsSdkDir 'Redist\D3D\x64\d3dcompiler_47.dll'
    if (Test-Path $sdkD3DCompiler) {
        Copy-Item $sdkD3DCompiler $Destination -Force
        Write-Host "Deployed d3dcompiler_47.dll from Windows SDK"
    }
}

# 3. Deploy downlevel UCRT forwarders if available
if ($env:WindowsSdkDir) {
    $ucrtDirs = Get-ChildItem (Join-Path $env:WindowsSdkDir 'Redist') -Recurse -Directory -Filter 'ucrt' -ErrorAction SilentlyContinue
    foreach ($u in $ucrtDirs) {
        $x64 = Join-Path $u.FullName 'DLLs\x64'
        if (Test-Path "$x64\ucrtbase.dll") {
            Copy-Item "$x64\*.dll" $Destination -Force
            Write-Host "Deployed UCRT DLLs from $x64"
            break
        }
    }
}

# 4. Deploy software rendering launch script
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmdScript = Join-Path $scriptDir 'HarmonyCast-SoftwareRendering.cmd'
if (Test-Path $cmdScript) {
    Copy-Item $cmdScript $Destination -Force
    Write-Host "Deployed HarmonyCast-SoftwareRendering.cmd"
}
