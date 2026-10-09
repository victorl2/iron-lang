<#
.SYNOPSIS
Iron installer for Windows.

.DESCRIPTION
Downloads the Iron release archive for Windows x86_64, unpacks it into
%USERPROFILE%\.iron (bin\iron.exe, bin\ironc.exe, lib\...) and adds
%USERPROFILE%\.iron\bin to the user's PATH. The C toolchain Iron compiles
with is downloaded by ironc itself on first use.

Usage (PowerShell):
  irm https://ironlang.dev/install.ps1 | iex
  .\install.ps1 -Version v4.6.0-alpha

.PARAMETER Version
A release tag (v4.6.0-alpha or 4.6.0-alpha). Default: the latest release.
#>
[CmdletBinding()]
param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$Repo = "victorl2/iron-lang"
$IronHome = Join-Path $env:USERPROFILE ".iron"
$BinDir = Join-Path $IronHome "bin"

if ([System.Environment]::Is64BitOperatingSystem -eq $false -or
    $env:PROCESSOR_ARCHITECTURE -notin @("AMD64")) {
    Write-Error "Iron ships Windows binaries for x86_64 only (this machine: $env:PROCESSOR_ARCHITECTURE)."
}
$Target = "windows-x86_64"

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if ($Version -eq "") {
    $latest = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ "User-Agent" = "iron-install" }
    $Version = $latest.tag_name
}
$Version = $Version.TrimStart("v")
$Archive = "iron-$Version-$Target.zip"
$Url = "https://github.com/$Repo/releases/download/v$Version/$Archive"

Write-Host "Installing iron $Version for $Target..."
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("iron-install-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
try {
    $ZipPath = Join-Path $Tmp $Archive
    Write-Host "Downloading $Url..."
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
    # Verify against the release's SHA-256 sidecar ("<hash>  <file>").
    $ShaPath = "$ZipPath.sha256"
    $HaveSha = $true
    try {
        Invoke-WebRequest -Uri "$Url.sha256" -OutFile $ShaPath -UseBasicParsing
    } catch {
        $HaveSha = $false
    }
    if ($HaveSha) {
        $Expected = (Get-Content $ShaPath -Raw).Trim().Substring(0, 64).ToLower()
        $Actual = (Get-FileHash $ZipPath -Algorithm SHA256).Hash.ToLower()
        if ($Expected -ne $Actual) {
            throw "checksum mismatch for ${Archive}: expected $Expected, got $Actual"
        }
        Write-Host "Checksum verified."
    } else {
        Write-Host "Note: no checksum published for $Archive; skipping verification."
    }
    # The archive holds bin\ and lib\ at its root, the same layout as ~/.iron.
    Expand-Archive -Path $ZipPath -DestinationPath $IronHome -Force
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}

# User PATH, persisted (new terminals) and for this session.
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (($UserPath -split ";") -notcontains $BinDir) {
    [Environment]::SetEnvironmentVariable("Path", "$BinDir;$UserPath", "User")
    Write-Host "  Added $BinDir to the user PATH"
}
if (($env:Path -split ";") -notcontains $BinDir) {
    $env:Path = "$BinDir;$env:Path"
}

Write-Host ""
& (Join-Path $BinDir "iron.exe") --version
Write-Host ""
Write-Host "Iron is installed. Open a new terminal, or use this one, and run:"
Write-Host "  iron --version"
Write-Host ""
Write-Host "Programs link against the runtime that ships with Iron, so neither"
Write-Host "Visual Studio nor its Build Tools is needed. The C compiler (a pinned"
Write-Host "LLVM) is downloaded by ironc on first use."
