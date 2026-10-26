# make_msix.ps1: Create the MSIX package of JoliPuTTY.
#
# Usually run by the build, after building jputty.exe:
#
#   cmake --build . --config Release --target msix
#
# but it can be run by hand too, giving it where jputty.exe was built
# (-ExeDir) and the Windows App SDK headers it was built with
# (-WinUIInclude), which it otherwise looks for in the current directory,
# taken as the build directory.
#
# Copies jputty.exe and the Windows App SDK bootstrapper DLL (which it
# links to, though it doesn't use it when it's run from the package) to
# $StagingDir, with the logos (msix\Assets) and the licence, fills
# msix\AppxManifest.xml.in, creates resources.pri with makepri.exe and packs
# it with makeappx.exe (both from the Windows SDK, e.g. in a Visual Studio
# Developer Command Prompt).
#
# The package is not signed.  To test it without signing, register the
# staging directory, with the Developer Mode enabled in Settings:
#
#   Add-AppxPackage -Register <StagingDir>\AppxManifest.xml
#
# The defaults of -Name, -Publisher and -PublisherName are those of the
# Partner Center account, so the package is ready to upload unsigned to the
# Microsoft Store (the Store signs it), once the name "JoliPuTTY" is
# reserved there.  To sideload a self-signed build instead, override
# -Publisher to match the test certificate's subject (e.g.
# -Publisher "CN=JoliPuTTY Test").

param(
  # Where jputty.exe and Microsoft.WindowsAppRuntime.Bootstrap.dll are
  # (by default, the build directory's Release subdirectory, or the build
  # directory itself).
  [string] $ExeDir = "",
  # Directory of WindowsAppSDK-VersionInfo.h, for the version of the
  # Windows App Runtime the package depends on (by default, from the NuGet
  # packages the build downloaded).
  [string] $WinUIInclude = "",
  # JoliPuTTY's version, as major.minor.build.0 (by default, the build's,
  # from joliputty-version.h, written by cmake/joliputty-version.cmake:
  # which must then be a release, on a joliputty-X.Y tag).
  [string] $Version = "",
  # Where joliputty-version.h is (by default, the build directory's
  # CMakeFiles).
  [string] $VersionHeader = "CMakeFiles\joliputty-version.h",
  [ValidateSet("x64", "arm64")] [string] $Arch = "x64",
  [string] $Name = "SylvainCresto.JoliPuTTY",
  [string] $Publisher = "CN=4D7AE54D-4897-4AAF-AD4A-96A83C99020F",
  [string] $PublisherName = "Sylvain Cresto",
  [string] $StagingDir = "msix\staging",
  [string] $OutFile = ""
)

$ErrorActionPreference = "Stop"

# The source directory's windows\msix, wherever this is run from.
$msixDir = $PSScriptRoot
$srcDir = Split-Path -Parent (Split-Path -Parent $msixDir)

# Find a Windows SDK tool, from PATH (Developer Command Prompt) or the SDK.
function Find-SdkTool([string] $tool) {
  $cmd = Get-Command $tool -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
  $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\$hostArch\$tool" `
             -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
  if (-not $found) { throw "$tool not found" }
  return $found.FullName
}

if (-not $ExeDir) {
  $ExeDir = if (Test-Path "Release\jputty.exe") { "Release" } else { "." }
}
$exes = "jputty.exe", "Microsoft.WindowsAppRuntime.Bootstrap.dll"
foreach ($f in $exes) {
  if (-not (Test-Path (Join-Path $ExeDir $f))) {
    throw "$f not found in $ExeDir, build jputty.exe first (or give -ExeDir)"
  }
}

if (-not $Version) {
  if (-not (Test-Path $VersionHeader)) {
    throw "$VersionHeader not found, build jputty.exe first (or give -VersionHeader or -Version)"
  }
  $h = Get-Content -Raw $VersionHeader
  $v = [regex]::Match($h, '#define\s+JOLIPUTTY_VERSION\s+"([^"]*)"').Groups[1].Value
  if (-not ($h -match '#define\s+JOLIPUTTY_RELEASE\s+1')) {
    throw "JoliPuTTY $v is not a release: the MSIX is only made on a joliputty-X.Y tag, with nothing uncommitted (or give -Version)"
  }
  # (MSIX versions have four parts, the last one 0 for the Store)
  $b = [regex]::Match($h, '#define\s+JOLIPUTTY_BINARY_VERSION\s+([0-9,]+)').Groups[1].Value
  $Version = (@($b -split ',')[0..2] + "0") -join "."
}
if (-not $OutFile) { $OutFile = "JoliPuTTY_${Version}_$Arch.msix" }

if (-not $WinUIInclude) {
  $found = Get-ChildItem "nuget\Microsoft.WindowsAppSDK.Runtime.*\include\WindowsAppSDK-VersionInfo.h" `
             -ErrorAction SilentlyContinue |
           Sort-Object { [version]($_.Directory.Parent.Name -replace '^Microsoft\.WindowsAppSDK\.Runtime\.', '') } |
           Select-Object -Last 1
  if (-not $found) {
    throw "WindowsAppSDK-VersionInfo.h not found under nuget, run this from the build directory (or give -WinUIInclude)"
  }
  $WinUIInclude = $found.DirectoryName
}

# Stage the package content.
if (Test-Path $StagingDir) { Remove-Item -Recurse -Force $StagingDir }
New-Item -ItemType Directory -Force "$StagingDir\msix" | Out-Null
foreach ($f in $exes) { Copy-Item (Join-Path $ExeDir $f) $StagingDir }
Copy-Item "$srcDir\LICENCE" $StagingDir
Copy-Item "$msixDir\Assets\*.png" "$StagingDir\msix"

# The manifest.
$manifest = Get-Content -Raw -Encoding UTF8 "$msixDir\AppxManifest.xml.in"
$manifest = $manifest.Replace("@NAME@", $Name)
$manifest = $manifest.Replace("@PUBLISHER@", [Security.SecurityElement]::Escape($Publisher))
$manifest = $manifest.Replace("@PUBLISHERNAME@", [Security.SecurityElement]::Escape($PublisherName))
$manifest = $manifest.Replace("@VERSION@", $Version)
$manifest = $manifest.Replace("@ARCH@", $Arch)

# The framework package of the Windows App Runtime used for building, e.g.
# Microsoft.WindowsAppRuntime.2 version 2.5.1.0.  The Store installs it
# with the package; when sideloading it must already be installed.
$info = Get-Content -Raw (Join-Path $WinUIInclude "WindowsAppSDK-VersionInfo.h")
$family = [regex]::Match($info, 'PACKAGE_FRAMEWORK_PACKAGEFAMILYNAME\s+"([^"_]+)_[^"]*"').Groups[1].Value
$minVer = [regex]::Match($info, 'RUNTIME_VERSION_DOTQUADSTRING\s+"([^"]+)"').Groups[1].Value
$pub = [regex]::Match($info, 'RUNTIME_IDENTITY_PUBLISHER\s+"([^"]+)"').Groups[1].Value
if (-not $family -or -not $minVer -or -not $pub) {
  throw "Windows App Runtime version not found in $WinUIInclude\WindowsAppSDK-VersionInfo.h"
}
$deps = "`r`n    <PackageDependency Name=`"$family`" MinVersion=`"$minVer`"`r`n" +
        "                       Publisher=`"$([Security.SecurityElement]::Escape($pub))`" />"
$manifest = $manifest.Replace("@DEPENDENCIES@", $deps)
Set-Content -Encoding UTF8 -Path "$StagingDir\AppxManifest.xml" -Value $manifest

# resources.pri: the variants of the logos (msix\Assets), indexed from a
# separate directory, so that only they are taken as resources.
$priRoot = "$StagingDir.pri"
if (Test-Path $priRoot) { Remove-Item -Recurse -Force $priRoot }
New-Item -ItemType Directory -Force "$priRoot\msix" | Out-Null
Copy-Item "$msixDir\Assets\*.png" "$priRoot\msix"
$makepri = Find-SdkTool makepri.exe
& $makepri createconfig /cf "$priRoot.xml" /dq en-US /pv 10.0.0 /o
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed" }
& $makepri new /pr $priRoot /cf "$priRoot.xml" `
    /mn "$StagingDir\AppxManifest.xml" /of "$StagingDir\resources.pri" /o
if ($LASTEXITCODE -ne 0) { throw "makepri failed" }

& (Find-SdkTool makeappx.exe) pack /o /d $StagingDir /p $OutFile
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
Write-Host "Created $OutFile (not signed)"
