# HighFPS Build & Package Script
Param (
    [string]$BuildType = "Release",
    [string]$NdkPath = "",
    [string]$CmakePath = "",
    [string]$NinjaPath = ""
)

$ErrorActionPreference = "Stop"

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  HighFPS Mod Build Script (Android ARM64)" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

# 0. Auto-detect Android NDK
if (-not $NdkPath) {
    $PotentialNdkPaths = @(
        $env:ANDROID_NDK_HOME,
        $env:ANDROID_NDK_ROOT,
        $env:NDK_HOME,
        "D:\Package\AndroidSDK\ndk\26.1.10909125"
    )
    if ($env:ANDROID_HOME -and (Test-Path "$env:ANDROID_HOME\ndk")) {
        $LatestNdk = Get-ChildItem "$env:ANDROID_HOME\ndk" -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($LatestNdk) { $PotentialNdkPaths += $LatestNdk.FullName }
    }
    if ($env:LOCALAPPDATA -and (Test-Path "$env:LOCALAPPDATA\Android\Sdk\ndk")) {
        $LatestNdk = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($LatestNdk) { $PotentialNdkPaths += $LatestNdk.FullName }
    }

    foreach ($Candidate in $PotentialNdkPaths) {
        if ($Candidate -and (Test-Path (Join-Path $Candidate "build/cmake/android.toolchain.cmake"))) {
            $NdkPath = $Candidate
            break
        }
    }
}

if (-not $NdkPath -or -not (Test-Path (Join-Path $NdkPath "build/cmake/android.toolchain.cmake"))) {
    Write-Error "Android NDK toolchain not found! Please set ANDROID_NDK_HOME or pass -NdkPath <path>."
}
Write-Host "Using Android NDK: $NdkPath" -ForegroundColor Gray

# Auto-detect CMake & Ninja
if (-not $CmakePath) {
    $CmakeCmd = Get-Command "cmake" -ErrorAction SilentlyContinue
    if ($CmakeCmd) {
        $CmakePath = $CmakeCmd.Source
    } elseif (Test-Path "D:\Package\AndroidSDK\cmake\3.31.6\bin\cmake.exe") {
        $CmakePath = "D:\Package\AndroidSDK\cmake\3.31.6\bin\cmake.exe"
    } else {
        $CmakePath = "cmake"
    }
}

if (-not $NinjaPath) {
    $NinjaCmd = Get-Command "ninja" -ErrorAction SilentlyContinue
    if ($NinjaCmd) {
        $NinjaPath = $NinjaCmd.Source
    } elseif (Test-Path "D:\Package\AndroidSDK\cmake\3.31.6\bin\ninja.exe") {
        $NinjaPath = "D:\Package\AndroidSDK\cmake\3.31.6\bin\ninja.exe"
    }
}

# Check NDK toolchain
$ToolchainFile = Join-Path $NdkPath "build/cmake/android.toolchain.cmake"

# 1. CMake Configure
$BuildDir = "build_$($BuildType.ToLower())"
Write-Host "[1/3] Configuring with CMake ($BuildType)..." -ForegroundColor Yellow

$CmakeArgs = @(
    "-B", $BuildDir,
    "-S", "src",
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
    "-DANDROID_ABI=arm64-v8a",
    "-DANDROID_PLATFORM=android-28",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-GNinja"
)
if ($NinjaPath) {
    $CmakeArgs += "-DCMAKE_MAKE_PROGRAM=$NinjaPath"
}

& $CmakePath @CmakeArgs

# 2. CMake Build
Write-Host "[2/3] Building target with Ninja..." -ForegroundColor Yellow
& $CmakePath --build $BuildDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed with code $LASTEXITCODE"
}

# Strip release binary
$BinaryPath = "$BuildDir/libhighfps.android.arm64.so"
$StripCandidates = @(
    (Join-Path $NdkPath "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"),
    (Join-Path $NdkPath "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"),
    (Join-Path $NdkPath "toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip")
)
$StripExe = $StripCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($BuildType -eq "Release" -and $StripExe) {
    Write-Host "      Stripping debug symbols..." -ForegroundColor Yellow
    & $StripExe --strip-all $BinaryPath
}

# 3. Packaging for TEFManager
Write-Host "[3/3] Creating distribution packages..." -ForegroundColor Yellow
if (-not (Test-Path "dist")) {
    New-Item -ItemType Directory -Force -Path "dist" | Out-Null
}

$DistModDir = "dist/top.morenrx.highfps"
if (Test-Path $DistModDir) {
    Remove-Item -Recurse -Force $DistModDir
}

# Remove any obsolete direct install directory
if (Test-Path "dist/mods_ready_to_install") {
    Remove-Item -Recurse -Force "dist/mods_ready_to_install"
}

New-Item -ItemType Directory -Force -Path "$DistModDir/resources/lib" | Out-Null

# Files for TEFManager import
Copy-Item -Path $BinaryPath -Destination "$DistModDir/resources/lib/" -Force
Copy-Item -Path "src/modinfo/Info.json" -Destination "$DistModDir/" -Force
Copy-Item -Path "src/modinfo/Manifest.json" -Destination "$DistModDir/" -Force
Copy-Item -Path "src/modinfo/mod.json" -Destination "$DistModDir/" -Force

# Create zip with forward slash separators for TEFManager import
$DistPath = (Resolve-Path "dist").Path
$ZipPath = Join-Path $DistPath "HighFPS_TEFKL_android_arm64.zip"
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ZipArchive = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
$BaseDirResolved = (Resolve-Path $DistModDir).Path
Get-ChildItem -Path $DistModDir -Recurse -File | ForEach-Object {
    $RelativePath = $_.FullName.Substring($BaseDirResolved.Length + 1).Replace('\', '/')
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($ZipArchive, $_.FullName, $RelativePath) | Out-Null
}
$ZipArchive.Dispose()

# Clean up staging directory
Remove-Item -Recurse -Force $DistModDir

Write-Host "==========================================" -ForegroundColor Green
Write-Host "  Build & Packaging Succeeded!" -ForegroundColor Green
Write-Host "  Package: $ZipPath" -ForegroundColor Green
Write-Host "==========================================" -ForegroundColor Green
