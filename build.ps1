# HighFPS Build & Package Script
Param (
    [string]$BuildType = "Release",
    [string]$NdkPath = "D:\Package\AndroidSDK\ndk\26.1.10909125",
    [string]$CmakePath = "D:\Package\AndroidSDK\cmake\3.31.6\bin\cmake.exe",
    [string]$NinjaPath = "D:\Package\AndroidSDK\cmake\3.31.6\bin\ninja.exe"
)

$ErrorActionPreference = "Stop"

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  HighFPS Mod Build Script (Android ARM64)" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

# Check NDK toolchain
$ToolchainFile = Join-Path $NdkPath "build/cmake/android.toolchain.cmake"
if (-not (Test-Path $ToolchainFile)) {
    Write-Error "Android NDK toolchain file not found at: $ToolchainFile"
}

# 1. CMake Configure
$BuildDir = "build_$($BuildType.ToLower())"
Write-Host "[1/3] Configuring with CMake ($BuildType)..." -ForegroundColor Yellow
& $CmakePath -B $BuildDir -S . `
    -DCMAKE_TOOLCHAIN_FILE="$ToolchainFile" `
    -DANDROID_ABI="arm64-v8a" `
    -DANDROID_PLATFORM="android-28" `
    -DCMAKE_BUILD_TYPE="$BuildType" `
    -GNinja `
    -DCMAKE_MAKE_PROGRAM="$NinjaPath"

# 2. CMake Build
Write-Host "[2/3] Building target with Ninja..." -ForegroundColor Yellow
& $CmakePath --build $BuildDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed with code $LASTEXITCODE"
}

# Strip release binary
$BinaryPath = "$BuildDir/libhighfps.android.arm64.so"
$StripExe = Join-Path $NdkPath "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
if ($BuildType -eq "Release" -and (Test-Path $StripExe)) {
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
Copy-Item -Path "Info.json" -Destination "$DistModDir/" -Force
Copy-Item -Path "Manifest.json" -Destination "$DistModDir/" -Force
Copy-Item -Path "mod.json" -Destination "$DistModDir/" -Force

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
