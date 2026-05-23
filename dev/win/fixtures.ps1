# fixtures.ps1 — native-Windows fixture download + verify + extract.
#
# Replaces the MSYS2-bound `make fixture` / `make verify-q3demo` /
# `make verify-goldsrc-*` flow for Windows CI. Uses only PowerShell 7+
# native cmdlets (Invoke-WebRequest, Get-FileHash, Expand-Archive)
# plus the tar.exe that ships with Windows 10+.
#
# URLs and SHA-256 pins MUST stay aligned with the Makefile at
# `Makefile:107-156`. The Unix path still uses the Makefile recipes;
# this is the parallel Windows-only implementation.
#
# Usage:
#   pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite quake
#   pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite q3
#   pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite goldsrc-uplink
#   pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite goldsrc-dayone

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('quake', 'q3', 'goldsrc-uplink', 'goldsrc-dayone')]
    [string]$Suite
)

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest's progress bar is pathologically slow on multi-MB
# downloads — silencing it cuts the Q3 fixture fetch from ~minutes to
# seconds. Microsoft documents this as expected behavior; -OutFile
# bypasses the buffered-pipeline path that triggers it.
$ProgressPreference = 'SilentlyContinue'

$RepoRoot = (Resolve-Path "$PSScriptRoot/../..").Path
$TestDir  = Join-Path $RepoRoot 'build/test'
New-Item -ItemType Directory -Path $TestDir -Force | Out-Null

# Per-fixture URL + SHA-256 pin + path layout. Keep aligned with Makefile.
$Fixtures = @{
    'quake' = @{
        Url       = 'https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz'
        Sha256    = 'd173e9f828b932a8160d4c65927281d0c28131cd922f0bf0d69e92a35185b499'
        Archive   = Join-Path $TestDir 'quakesw.tar.gz'
        InnerPath = 'id1/pak0.pak'
        OutPath   = Join-Path $TestDir 'pak0.pak'
        # .tar.gz — tar.exe extracts; the wrapper has id1/pak0.pak.
        Kind      = 'targz'
    }
    'q3' = @{
        Url       = 'https://archive.org/download/Q3A-Demo/Quake%203%20Arena%20Demo.zip'
        Sha256    = 'e9f89ef064317634aab3b3a3add131887967fc04744526bd624e1914b1e25b3e'
        Archive   = Join-Path $TestDir 'q3demo.zip'
        InnerPath = 'Quake 3 Arena Demo/demoq3/pak0.pk3'
        OutPath   = Join-Path $TestDir 'q3demo/pak0.pk3'
        Kind      = 'zip'
    }
    'goldsrc-uplink' = @{
        Url       = 'https://archive.org/download/half-life-day-one/Half-LifeUplink.zip'
        Sha256    = '6e06a9f25d36ec12750da8f94af24a71f26af2330a665a9d4922421db4459aa4'
        Archive   = Join-Path $TestDir 'hl-uplink.zip'
        # Uplink's inner pak is uppercase .PAK; the canonical out path
        # we expose to tests is lowercase to match the Day One layout.
        InnerPath = 'Half-LifeUplink/valve/pak0.PAK'
        OutPath   = Join-Path $TestDir 'hl-uplink/valve/pak0.pak'
        Kind      = 'zip'
    }
    'goldsrc-dayone' = @{
        Url       = 'https://archive.org/download/half-life-day-one/Half-Life%20Day%20One.zip'
        Sha256    = '35098523a078cd2cde858a261e7071f1e1e79ae0c38fac9302186d3cd9bd001d'
        Archive   = Join-Path $TestDir 'hl-dayone.zip'
        InnerPath = 'Half-Life Day One/valve/pak0.pak'
        OutPath   = Join-Path $TestDir 'hl-dayone/valve/pak0.pak'
        Kind      = 'zip'
    }
}

$F = $Fixtures[$Suite]

function Get-Sha256OrEmpty([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLower()
}

# Step 1: download (if missing or wrong SHA).
$existingSha = Get-Sha256OrEmpty -Path $F.Archive
if ($existingSha -ne $F.Sha256) {
    Write-Host "==> Downloading $($F.Url)"
    Invoke-WebRequest -Uri $F.Url -OutFile $F.Archive -UseBasicParsing

    $actual = Get-Sha256OrEmpty -Path $F.Archive
    if ($actual -ne $F.Sha256) {
        Remove-Item -LiteralPath $F.Archive -ErrorAction SilentlyContinue
        throw "SHA256 mismatch on $($F.Archive): expected $($F.Sha256), got $actual"
    }
} else {
    Write-Host "==> Cached: $($F.Archive) (SHA256 matches)"
}

# Step 2: extract if the inner pak isn't already in place.
if (Test-Path -LiteralPath $F.OutPath) {
    Write-Host "==> Cached: $($F.OutPath)"
    exit 0
}

# Stage extraction in a per-fixture "_raw" sibling so a partial run
# doesn't leave half-extracted state in the canonical layout. Clean it
# first to be safe across reruns.
$RawDir = Join-Path $TestDir ($Suite + '_raw')
if (Test-Path -LiteralPath $RawDir) {
    Remove-Item -LiteralPath $RawDir -Recurse -Force
}
New-Item -ItemType Directory -Path $RawDir -Force | Out-Null

if ($F.Kind -eq 'zip') {
    # Expand-Archive is ZIP-only; uses System.IO.Compression.ZipArchive
    # underneath. -LiteralPath handles wrapper names with spaces /
    # brackets / etc. without glob expansion.
    Expand-Archive -LiteralPath $F.Archive -DestinationPath $RawDir -Force
} elseif ($F.Kind -eq 'targz') {
    # tar.exe ships with Windows 10+; the GHA windows-2025 image has it.
    # Use -xzf so gzip decompression happens in one shot.
    & tar -xzf $F.Archive -C $RawDir
    if ($LASTEXITCODE -ne 0) {
        throw "tar -xzf failed for $($F.Archive) (exit $LASTEXITCODE)"
    }
} else {
    throw "Unknown fixture kind: $($F.Kind)"
}

# Copy the inner pak to the canonical path tests expect.
$Inner = Join-Path $RawDir $F.InnerPath
if (-not (Test-Path -LiteralPath $Inner)) {
    throw "Expected inner fixture not found at $Inner"
}
$OutDir = Split-Path -Parent $F.OutPath
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Copy-Item -LiteralPath $Inner -Destination $F.OutPath -Force

Write-Host "==> Ready: $($F.OutPath)"
