# Pinned fixture URLs + SHA-256 hashes. Single source of truth for both
# the Makefile (which `include`s this file) and dev/win/fixtures.ps1
# (which parses these KEY=VALUE pairs at runtime).
#
# Edit ONLY this file to point at new sources. The CI cache keys in
# .github/workflows/test.yml hash this file, so changes here invalidate
# the runner-side fixture cache and force a re-download on the next
# workflow run; recipe / comment edits elsewhere in the Makefile do
# NOT bust the cache.

# id Software, Quake 1 shareware (libsdl.org mirror — Sam Lantinga's
# long-term home for the redistributable demo set).
QUAKE_URL=https://www.libsdl.org/projects/quake/data/quakesw-1.0.6.tar.gz
QUAKE_SHA256=d173e9f828b932a8160d4c65927281d0c28131cd922f0bf0d69e92a35185b499

# id Software, Quake 3 Arena demo (archive.org community mirror —
# redistribution of id's freely distributable demo). Used by the
# optional `realpak-test-q3` target only.
Q3DEMO_URL=https://archive.org/download/Q3A-Demo/Quake%203%20Arena%20Demo.zip
Q3DEMO_SHA256=e9f89ef064317634aab3b3a3add131887967fc04744526bd624e1914b1e25b3e

# Valve, Half-Life Uplink (1999 free standalone demo; archive.org mirror).
GOLDSRC_UPLINK_URL=https://archive.org/download/half-life-day-one/Half-LifeUplink.zip
GOLDSRC_UPLINK_SHA256=6e06a9f25d36ec12750da8f94af24a71f26af2330a665a9d4922421db4459aa4

# Valve, Half-Life: Day One (1998 OEM Voodoo-card demo; same archive.org item).
GOLDSRC_DAYONE_URL=https://archive.org/download/half-life-day-one/Half-Life%20Day%20One.zip
GOLDSRC_DAYONE_SHA256=35098523a078cd2cde858a261e7071f1e1e79ae0c38fac9302186d3cd9bd001d
