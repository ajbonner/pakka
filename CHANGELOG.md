# Changelog

All notable changes to pakka are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.7.0] — 2026-05-24

### Added
- Pre-built Windows **x86** zip alongside the existing x64 — both built
  with MSVC on the `windows-2025-vs2026` runner.
- Pre-built macOS Intel (`x86_64`) and Apple Silicon (`arm64`) archives,
  shipped in both `.tar.gz` and `.zip`. Native runners for each target
  — no cross-compile, no universal-binary merge.
- This `CHANGELOG.md`. The project follows
  [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) going forward.

### Changed
- README slimmed to a launching point that defers detail to the
  published documentation site at
  <https://ajbonner.github.io/pakka/>; supporting reference pages
  (formats, install, libpakka, platforms, security, testing) live
  under `docs/`. The opening paragraph now leads with libpakka — the
  durable C library surface — rather than enumerating mods.

### Fixed
- GitHub Pages workflow actions bumped off Node 20 ahead of the
  forced-Node-24 cutover: `actions/configure-pages` v5 → v6,
  `actions/upload-pages-artifact` v3 → v5, `actions/deploy-pages`
  v4 → v5.

## [1.6.0] — 2026-05-24

### Added
- First release-workflow run that attaches binary artifacts to the
  GitHub release: source tarball (`pakka-X.Y.Z.tar.gz`) and a single
  Windows x64 zip (`pakka-X.Y.Z-windows-x64.zip`), each with a
  SHA-256 sidecar. (1.7.0 fans this out into the full Windows x86 +
  macOS Intel / arm64 matrix.)
- `make install` honouring the GNU install conventions (`PREFIX`,
  `DESTDIR`). Installs `bin/pakka`, `lib/libpakka.a`, `include/pakka.h`,
  and manpages under `share/man/`.
- `pakka(1)` and `pakka(3)` manpages.
- macOS arm64 realpak CI jobs — real-world archive coverage now runs on
  all three modern targets (Linux, Windows, macOS Intel + arm64).
- Native Windows fixture preparation via `dev/win/fixtures.ps1`, the
  PowerShell equivalent of `make fixture` / `make verify-*`.

### Changed
- The entire test suite is now native C driven by CTest. The bats
  harness is gone from the build and CI; Windows tests no longer need
  MSYS2 — they run under `pwsh` + `ctest` against the same binaries
  shipped in releases.
- CI realpak jobs reuse the pakka + test binaries built in the main job
  instead of rebuilding per-suite.
- Fixture URLs and SHA-256 pins extracted to `dev/fixtures.mk` so cache
  keys stay stable across both Makefile and PowerShell paths.
- The on-disk test directory was renamed from `tests/` to `test/`.

### Fixed
- Windows abort() dialog hang in CI. CTest now enforces a 120 s timeout.
- AddressSanitizer leaks and an MSVC unreachable-code error surfaced by
  the new C test suite.
- UTF-8 path handling completed on Windows: wide-char directory walk
  plus `remove` / `rmdir` go through the same conversion layer.

## [1.5.0] — 2026-05-22

### Added
- Doom IWAD and PWAD support — `pakka -l/-x/-c/-a/-d/--verify` all
  recognise WAD lump archives. `.wad` defaults to PWAD on create; use
  `--format iwad` for the base-game flavour.
- DEFLATE compression on PK3 / PK4 write. Pakka bundles the `sdefl` +
  `sinfl` codec; `--compress` enables it. PK3 / PK4 still store entries
  uncompressed by default, and pakka falls back to STORED per-entry
  when DEFLATE wouldn't shrink the input. An opt-in `zlib` backend is
  available at build time.
- UTF-8 path handling on Windows (initial CLI-side coverage).
- Compressed-size field on `pakka_entry_info`.
- libFuzzer harnesses, a coverage target, and a clang-tidy lint pass on
  the `_WIN32` arm via Mingw cross-headers.
- Realpak opt-in test suites that drive pakka against real game-engine
  archives — Q3 demo `pak0.pk3`, Half-Life Uplink and Day One
  `valve/pak0.pak`. Gated on env vars pointing at the downloaded files.

### Changed
- Daikatana support is now read **and** write, including create / add /
  delete; the encoder uses the original Ion Storm packer's add-time
  policy (`.tga`, `.bmp`, `.wal`, `.pcx`, `.bsp` compressed; everything
  else stored).
- ZIP / PK3 add path made atomic: writes go to a temp archive that
  replaces the original via rename, with rebuild rollback if the
  rename fails. Tightened EOCD tail handling, CDR record validation,
  and allocation caps.

### Fixed
- Big-endian DEFLATE decode in `sinfl`; BE-stable hash in `sdefl`.
- An out-of-bounds read in `sinfl`, gcc 3.x portability gaps, and
  FreeBSD test robustness.
- Windows extract CRT-shutdown hang caused by a double-free of the
  UTF-8 `argv` array after long-option stripping.
- GNU make 3.79.1 compatibility for the backend selection block.

## [1.4.0] — 2026-05-19

### Added
- SiN (Ritual, 1998) full read / write support — SPAK magic, 120-byte
  filenames, 128-byte directory entries. Auto-detected from the leading
  four bytes.
- Daikatana (Ion Storm, 2000) read support, including the custom
  RLE + back-reference byte-codec used for art assets. The decoder is
  modelled on the Yamagi Quake II team's `pakextract`.
- Quake 3 PK3 archive support (read / write / verify).
- Doom 3 PK4 archive support (read / write / verify).
- `--format <name>` flag — `pak | sin | daikatana | pk3 | pk4 | auto`
  — pins the format at open (skips the auto-probe) and overrides the
  extension sniffer at create.
- `-V` / `--version` flag, printing the pakka binary's version, the
  linked libpakka version, the build date, and the supported-format
  matrix.
- `--help` as a long alias for `-h`.
- `.sin` extension recognised at create time.
- libpakka API additions: `pakka_open_ex()` with explicit format hint,
  and `pakka_version()`. Public symbol count grew from 21 to 23.

### Changed
- **Breaking**: the archive path is now the first positional argument.
  The `-f` flag is gone. Pakka does no streaming I/O, so `-f` carried
  no information; the cleaner ergonomics matter more now that the tool
  has the polish to invite new users.
- `pakka_set_max_decompressed_size()`'s cap now applies to Daikatana
  compressed entries in addition to PK3 / PK4 DEFLATE.
- Per-format geometry now lives in a runtime table
  (`pakka_pak_geometry`) instead of compile-time
  `PAKFILE_DIR_ENTRY_SIZE` / `PAKFILE_PATH_MAX` constants.

### Removed
- The `-f` flag (see Changed above).
- The 2 GiB file-size ceiling on 32-bit POSIX and Windows MSVC builds.

## [1.3.0] — 2026-05-19

### Changed
- libpakka extracted from the monolithic CLI. `libpakka.a` now exposes
  an opaque-handle C99 API for opening, listing, extracting from, and
  mutating pak archives — never calling `exit`, never writing to
  stdout / stderr, returning structured errors. The `pakka(1)` binary
  is a thin wrapper that links against it.
- `src/main.c` renamed to `src/cli.c` to reflect its new role.

## [1.2.0] — 2026-05-17

### Added
- Windows / MSVC build support, driven by CMake.
- Red Hat 9-era legacy build floor — glibc 2.3+, gcc 3.0+, make 3.79.1+
  — exercised by a per-push Docker CI job.
- NetBSD 3.0 / sparc legacy probe, exercised by a per-push QEMU CI job
  to keep the pre-`openat` BSD path honest.
- Big-endian byte-order correctness on the PAK on-disk format.
- Advisory locks via `fcntl(F_SETLK)` so concurrent pakka invocations
  on the same archive can't corrupt each other.
- GitHub Actions release workflow that fires on `v*.*.*` tag pushes.

### Changed
- Extract is now symlink-safe end-to-end: every directory descent uses
  `O_NOFOLLOW`, an `fchdir` emulation on legacy POSIX, or a
  `GetFileAttributesA` check on Windows.
- CLI input validation tightened; errno stability fixed across error
  paths; print formatting hardened.
- Pak header and directory-entry validation hardened against malformed
  inputs.
- Documented Windows MSVC build, path-traversal hardening, and the
  Windows test recipe.

### Security
- Path-traversal entry names rejected at extract time (absolute paths,
  `..` components, drive letters, UNC).
- Windows-hostile entry-name policy: control bytes, colons, trailing
  dot / space, and reserved device names (`CON`, `NUL`, `COM0`–`COM9`,
  `LPT0`–`LPT9`) all rejected.
- Normalized-collision preflight refuses extractions where two entries
  would resolve to the same path on case-insensitive filesystems.

### Fixed
- Documented the 2–4 GiB pak limit on 32-bit POSIX and Windows MSVC
  (this release notes it; 1.4.0 lifts it).

## [1.1.0] — 2026-05-16

### Added
- `--tree` option for `pakka -l` — renders archive contents as a
  hierarchical UTF-8 directory tree.

## [1.0.0] — 2026-05-16

First tagged release of the modernized pakka.

### Added
- Multi-platform CI matrix, green on macOS Intel + Apple Silicon,
  Ubuntu glibc (x86_64 / arm64 / -m32), Alpine musl, FreeBSD 15
  (amd64 / -m32), and OpenBSD 7.8.
- Warning-free build under `-Wall --std=c99 --pedantic`.
- Integration test suite covering round-trip, list, extract, create,
  add, and delete across the Quake PAK format.

### Fixed
- Several real bugs surfaced during the warning cleanup.

## [0.x] — 2015-12 – 2016-01

The original pakka — created as an excuse to re-learn some C. Never
tagged a release; this entry captures the state of the project before
the 2026 modernization pass picked it up again.

### Added
- Read, list, create, add, and rudimentary delete operations for the
  Quake 1 / 2 PAK format.
- POSIX build via a hand-rolled `Makefile`.
- MIT licence.

[Unreleased]: https://github.com/ajbonner/pakka/compare/v1.7.0...HEAD
[1.7.0]: https://github.com/ajbonner/pakka/compare/v1.6.0...v1.7.0
[1.6.0]: https://github.com/ajbonner/pakka/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/ajbonner/pakka/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/ajbonner/pakka/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/ajbonner/pakka/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/ajbonner/pakka/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/ajbonner/pakka/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/ajbonner/pakka/releases/tag/v1.0.0
