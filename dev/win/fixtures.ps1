# fixtures.ps1 — native-Windows fixture download + verify + extract.
#
# PowerShell-native equivalent of the Makefile's fixture flow
# (`make fixture` / `make verify-q3demo` / `make verify-goldsrc-*`).
# Uses only PowerShell 7+ native cmdlets (Invoke-WebRequest,
# Get-FileHash, Expand-Archive) plus the tar.exe that ships with
# Windows 10+.
#
# URLs + SHA-256 pins are parsed from dev/fixtures.mk so this script
# and the Makefile share one source of truth. Path layout + extraction
# shape stay here because they're Windows-specific (canonical lowercase
# pak0.pak, tar.exe vs Expand-Archive choice).
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

# Parse dev/fixtures.mk into a hashtable. The file is plain KEY=VALUE
# lines (Make-syntax assignments); blank lines and `#` comments skipped.
$Pins = @{}
$pinFile = Join-Path $RepoRoot 'dev/fixtures.mk'
Get-Content -LiteralPath $pinFile | ForEach-Object {
    $line = $_.Trim()
    if ($line -eq '' -or $line.StartsWith('#')) { return }
    if ($line -match '^([A-Z0-9_]+)=(.+)$') {
        $Pins[$matches[1]] = $matches[2]
    }
}

# Per-fixture mapping: URL + SHA pin come from dev/fixtures.mk; the
# rest (archive path, inner path inside the wrapper, canonical output
# path, extraction kind) is Windows-side layout.
$Fixtures = @{
    'quake' = @{
        Url       = $Pins['QUAKE_URL']
        Sha256    = $Pins['QUAKE_SHA256']
        Archive   = Join-Path $TestDir 'quakesw.tar.gz'
        InnerPath = 'id1/pak0.pak'
        OutPath   = Join-Path $TestDir 'pak0.pak'
        # .tar.gz — tar.exe extracts; the wrapper has id1/pak0.pak.
        Kind      = 'targz'
    }
    'q3' = @{
        Url       = $Pins['Q3DEMO_URL']
        Sha256    = $Pins['Q3DEMO_SHA256']
        Archive   = Join-Path $TestDir 'q3demo.zip'
        InnerPath = 'Quake 3 Arena Demo/demoq3/pak0.pk3'
        OutPath   = Join-Path $TestDir 'q3demo/pak0.pk3'
        Kind      = 'zip'
    }
    'goldsrc-uplink' = @{
        Url       = $Pins['GOLDSRC_UPLINK_URL']
        Sha256    = $Pins['GOLDSRC_UPLINK_SHA256']
        Archive   = Join-Path $TestDir 'hl-uplink.zip'
        # Uplink's inner pak is uppercase .PAK; the canonical out path
        # we expose to tests is lowercase to match the Day One layout.
        InnerPath = 'Half-LifeUplink/valve/pak0.PAK'
        OutPath   = Join-Path $TestDir 'hl-uplink/valve/pak0.pak'
        Kind      = 'zip'
    }
    'goldsrc-dayone' = @{
        Url       = $Pins['GOLDSRC_DAYONE_URL']
        Sha256    = $Pins['GOLDSRC_DAYONE_SHA256']
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
