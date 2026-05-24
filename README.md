# Pakka
A command line utility for working with Quake 1 / 2 and GoldSrc
(Half-Life 1, Counter-Strike 1.6, TFC, ...) `.pak` files, SiN `.sin`
archives, Daikatana `.pak` archives, Quake 3 `.pk3` files, and Doom 3
`.pk4` files.

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden](http://www.inthenightgarden.co.uk/) character.

## Disclaimer
I am a novice C programmer. This code is the result of a learning exercise.
Given that C programs can be very brittle, and behave in unexpected ways,
I would strongly suggest you do not use this software :) If you are feeling
very brave and do choose to use this software and find a problem, please let
me know!

## Installation

### From the source tarball (Unix)

Each tagged release attaches a `pakka-X.Y.Z.tar.gz` source tarball and a
SHA-256 sidecar to the [GitHub release](https://github.com/ajbonner/pakka/releases).
Grab them, verify the checksum, build, and install:

    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.5.0/pakka-1.5.0.tar.gz
    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.5.0/pakka-1.5.0.tar.gz.sha256
    $ shasum -a 256 -c pakka-1.5.0.tar.gz.sha256
    $ tar xzf pakka-1.5.0.tar.gz
    $ cd pakka-1.5.0
    $ make

Then pick one install flavor:

    $ sudo make install                    # system-wide to /usr/local
    $ make install PREFIX=$HOME/.local     # user-only, no root
    $ make install DESTDIR=/tmp/stage      # packager staging (.deb / .rpm / pkg)

`PREFIX` defaults to `/usr/local`, which is root-owned on most systems —
plain `make install` will fail with "permission denied" without `sudo` or
a user-writable `PREFIX`. `make uninstall` reverses any of the above
(pass the same `PREFIX` / `DESTDIR` you installed with).

Installed layout (Unix conventions, via the GNU install dirs):

| File | Path |
|---|---|
| `pakka` (CLI) | `$(PREFIX)/bin/pakka` |
| `libpakka.a` (static lib) | `$(PREFIX)/lib/libpakka.a` |
| `pakka.h` (public header) | `$(PREFIX)/include/pakka.h` |
| `pakka(1)` (manpage) | `$(PREFIX)/share/man/man1/pakka.1` |
| README + LICENSE | `$(PREFIX)/share/doc/pakka/` |

Requires GNU make and a C99 compiler. On BSD systems install `gmake` and
use `gmake` in place of `make`. The build is dependency-free at runtime —
the DEFLATE codec is bundled. `PAKKA_DEFLATE_BACKEND=zlib` opts into the
system `libz` instead, for integrators who already link it.

### From the Windows zip

Releases also ship `pakka-X.Y.Z-windows-x64.zip`. Unzip it anywhere; add
the extracted `bin\` directory to your `PATH`, or copy `bin\pakka.exe`
into a directory already on `PATH`. The zip layout matches the Unix
install:

    pakka-1.5.0-windows-x64\
      bin\pakka.exe
      lib\pakka.lib          # static lib for integrators
      include\pakka.h        # public C header
      share\man\man1\pakka.1
      share\doc\pakka\README.md
      share\doc\pakka\LICENSE

### From source (git clone)

For contributors:

    $ git clone https://github.com/ajbonner/pakka.git
    $ cd pakka
    $ make

`make` produces two artifacts:

* `./pakka` — the command-line binary documented below.
* `build/lib-prod/libpakka.a` — a static library exposing pakka's
  archive operations as a C99 API. The public header is at
  `include/pakka.h`; every exported symbol is prefixed `pakka_` and
  `make symbol-audit` fails the build if anything else leaks out. See
  the C library section below.

`make dist` produces the release tarball locally (`pakka-X.Y.Z.tar.gz` +
SHA-256 sidecar) via `git archive HEAD`.

### Building on Windows (MSVC)

The Makefile is Unix-only. On Windows, build via CMake + `cl.exe` from a
Developer Command Prompt (or after running `vcvarsall.bat x64`):

    > cmake -B build\cmake -G Ninja .
    > cmake --build build\cmake

Produces `build\cmake\pakka.exe`. The CMake build is additive — it compiles
the same `src/*.c` (including `src/pk3file.c` and the vendored
`src/vendor/sdefl` / `src/vendor/sinfl` DEFLATE codec, or `<zlib.h>`
with `-DPAKKA_USE_ZLIB=ON`) plus the `src/vendor/wingetopt` (getopt)
and `src/vendor/dirent` (opendir/readdir) polyfills under `_WIN32`.

Supported Windows floor is **Windows XP SP3** (built with the VS 2017
`v141_xp` toolset for releases targeting XP/Vista/7; CI uses VS 2026
on Windows Server 2025). The source intentionally avoids API calls
that require later Windows versions so the same tree builds against
either toolset.

## Usage
Pakka has 6 major modes (one per invocation), each working on `.pak`
(Quake 1 / 2 and GoldSrc — Half-Life 1, CS 1.6, TFC, Sven Co-op, ...),
`.sin` (SiN), Daikatana `.pak`, `.pk3` (Quake 3), and `.pk4` (Doom 3)
archives. GoldSrc PAKs are bit-identical to Quake/Q2
PAKs (same `"PACK"` magic, 56-byte names, 64-byte directory entries),
so `--format pak` (or its `goldsrc` / `hl` aliases) covers them with
the same code path. PK3 and PK4 are byte-identical ZIP containers; the
extension only changes the label returned by `pakka_format()`. Daikatana
shares Quake's `"PACK"` magic — pakka probes both directory layouts at
open time, and `--format daikatana` pins the decision when the archive
is ambiguous.

The archive path is the first positional argument; any further
positionals are entry names (for `-x` / `-d`) or source files (for `-a`
/ `-c`). All option flags (including `-C <dest>`) must come before the
archive — pakka's getopt is POSIX-strict and stops scanning at the
first positional.

* List pak contents: `./pakka -l <archive>`
  * `--tree` renders the listing as a UTF-8 box-drawing directory tree.
* Extract: `./pakka -x [-C <destination>] <archive> [path...]`
  * With no path arguments, every entry is extracted.
  * With one or more path arguments, only those entries are extracted; pakka
    errors if any requested path isn't in the archive.
  * `-C` selects a destination directory; defaults to the current working
    directory.
* Create: `./pakka -c <archive> [file/dir...] [--as <entry_name> <source_path> ...]`
  * Format is picked from the destination extension (case-insensitive):
    `.pk3` → PK3, `.pk4` → PK4, `.sin` → SiN, anything else → PAK.
    `--format <name>` (`pak` — also `goldsrc` / `hl`, `sin`,
    `daikatana`, `pk3`, `pk4`) overrides the extension.
* Add to archive: `./pakka -a <archive> [file/dir...] [--as <entry_name> <source_path> ...]`
  * `--as <entry_name> <source_path>` adds a single file under an explicit
    entry name (the source path on disk and the name stored in the archive
    can differ). Repeatable; may be mixed with plain path arguments.
* Delete from archive: `./pakka -d <archive> [path...]`
* Verify: `./pakka --verify <archive>`
  * Walks every entry, runs the same name-safety check used at extract
    time, streams each payload to confirm the directory's `offset`/`length`
    point at readable bytes, and flags entries that would collide after
    portable-union normalization (case fold, slash/backslash, trailing
    dot/space). Exits non-zero on any error-level finding.

`./pakka -h` (or `--help`) prints a usage summary; `./pakka -V` (or
`--version`) prints the linked libpakka version, build date, and the
supported-format matrix.

### Format support

| Format | Read | List | Extract | Create | Add | Delete | Verify |
|---|---|---|---|---|---|---|---|
| Quake 1 / 2 PAK (also GoldSrc: Half-Life 1, CS 1.6, TFC, Sven Co-op) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| SiN (`SPAK`, 120-byte names) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Daikatana (`PACK`, 72-byte entries, custom codec) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Quake 3 PK3 (STORED + DEFLATE) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Doom 3 PK4 (STORED + DEFLATE) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

PK3/PK4 write defaults to STORED (method 0). Pass `--compress` to the
CLI, or call `pakka_set_compression(archive, PAKKA_COMPRESSION_DEFLATE,
&err)` from the library, to DEFLATE-encode new entries instead. Per-
entry auto-fallback to STORED when DEFLATE wouldn't beat STORED — see
the "DEFLATE write" section below.

Daikatana write support uses `pakka_dk_deflate` (greedy LZSS-style
encoder over the format's zero-run / byte-RLE / back-reference / literal
opcode set). Add-time policy: entries whose name ends in `.tga` /
`.bmp` / `.wal` / `.pcx` / `.bsp` are encoded, everything else is
STORED — this is the documented policy from the pakextract author's
format notes (Daniel Gibson). Per-entry auto-fallback to STORED when
the encoder doesn't shrink the source. Compressed entries decode through
`pakka_dk_inflate` (reference: yquake2/pakextract).

Refused for PK3/PK4 archives: ZIP64, encrypted entries, data descriptors
(general-purpose bit 3), multi-disk archives, and compression methods
other than STORED (0) and DEFLATE (8). Each refusal returns
`PAKKA_ERR_UNSUPPORTED` with a clear message.

### Using libpakka from C

See `pakka(3)` (installed at `${MANDIR}/man3/pakka.3` by `make install`,
or `man/pakka.3` in this tree) for the full API reference.

`include/pakka.h` exposes 24 functions for opening, inspecting,
extracting from, and mutating pak archives:

* Version: `pakka_version` returns the linked libpakka version string
  (e.g. `"1.5.0"`), useful for bindings that want to feature-gate on
  the loaded library.
* Archive lifecycle: `pakka_open` / `pakka_open_ex` (lets the caller
  pin the format) / `pakka_create` / `pakka_close` (close implicitly
  commits on dirty)
* Read introspection: `pakka_format` / `pakka_entry_count` /
  `pakka_entry_at` / `pakka_find_entry`
* Entry accessors over the opaque `pakka_entry_t`:
  `pakka_entry_name` / `pakka_entry_size` / `pakka_entry_offset`
* Streaming reads: `pakka_open_entry` / `pakka_reader_read` /
  `pakka_reader_close`
* Mutation: `pakka_add_file` (with separate source path + entry name) /
  `pakka_add_memory` / `pakka_delete` / `pakka_commit`
* Integrity: `pakka_verify` (drives a caller-supplied
  `pakka_report_fn` callback)
* Tuning: `pakka_set_max_decompressed_size` (caps PK3/PK4 DEFLATE and
  Daikatana decompressed payload size; default 64 MiB, 0 disables) /
  `pakka_set_compression` (PK3/PK4 only; switches new adds between
  STORED and DEFLATE — see "DEFLATE write" below)
* Memory convenience: `pakka_read_entry_alloc` / `pakka_free`

Library functions never call `exit` and never write to stdout/stderr;
every failure returns a `pakka_status_t` and optionally populates a
caller-provided `pakka_error_t` with structured detail (errno or Win32
`GetLastError`, operation name, entry index, file offset, message).

For archives with compressed entries (PK3/PK4 DEFLATE and Daikatana),
`pakka_set_max_decompressed_size(archive, max_bytes, err)` caps the
bytes any single `pakka_open_entry` / `pakka_read_entry_alloc` will
inflate — refuses zip-bomb-style high-ratio entries before they hit
RAM. Default 64 MiB; pass 0 to disable. `pakka_verify` with the
`PAKKA_VERIFY_DEEP` flag adds per-entry CRC32 and decompression checks
on top of the structural walk (CRC for PK3/PK4; byte-count check for
Daikatana — the custom codec has no CRC).

### DEFLATE write (PK3/PK4)

Both `pakka -c --compress` (CLI) and `pakka_set_compression(archive,
PAKKA_COMPRESSION_DEFLATE, err)` (library) enable DEFLATE on new
entries added to a PK3/PK4 archive. Default is STORED; the setter is
sticky on the open handle and rejected on non-ZIP archives. Each
entry's compressed payload is computed up front; if DEFLATE doesn't
beat STORED for that entry (incompressible inputs like PNG, MP3, or
random bytes), the archive falls back to STORED for that entry
automatically — matches info-zip behavior.

The DEFLATE codec is selectable at build time. The default is the
bundled `sdefl` + `sinfl` single-header encoder/decoder pair (no
external dependency, ~1400 LOC vendored under `src/vendor/`):

    make                            # default: bundled codec
    cmake -B build/cmake .          # same on Windows

Game-engine integrators who already link `zlib` can route DEFLATE
through it instead and skip the duplicate codec code:

    make PAKKA_DEFLATE_BACKEND=zlib
    cmake -B build/cmake -DPAKKA_USE_ZLIB=ON .

Both backends produce byte-interchangeable raw RFC 1951 DEFLATE
streams — a PK3 written with one backend reads cleanly with the
other, and with any third-party tool (info-zip, Windows Explorer,
Quake III / Doom 3 engines).

`test/c_api_test.c` exercises every one of those functions against
`libpakka.a` only (no internal headers) and is the canonical example
of call patterns.

## Security

Pak entry names are attacker-controlled data (up to 56 bytes for Quake
PAK / Daikatana, 120 bytes for SiN, up to the ZIP cap for PK3/PK4).
Extract, add, and verify share a fail-fast validator
(`pakka_unsafe_entry_name`) that is format-independent — it inspects
the byte content, not the field width — and rejects entry names that
would escape the destination or materialize as something dangerous on
disk. The same rules apply to the entry-name side of `pakka_add_file`
/ `--as` so a pak built with pakka can be re-extracted with pakka
without surprises.

Rejected entry names:

* empty names
* names beginning with `/` or `\` (absolute paths)
* drive-letter prefixes (`C:...`, `D:...`) and UNC `\\server\share`
* any path component that is exactly `..` (slash- or backslash-separated)
* any path component containing a colon (Windows alternate-data-stream
  hazard, e.g. `file:stream`)
* any path component ending in `.` or space (Windows silently strips
  these, so `foo.` and `foo` would collide)
* control bytes (`0x00`-`0x1F` or `0x7F`) anywhere in the name
* Windows reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`), including with extensions (`NUL.txt`) and in
  subdirectories (`foo/CON`). `COM10` is allowed — only `COM0`-`COM9`
  are reserved.

Substring matches like `foo..bar` or `..png` remain legal — only an exact
`..` component is rejected. Validation runs before any `mkdir`, `fread`,
or `fopen`, so a malicious pak fails fast with no partial writes. Both
POSIX and Windows traversal forms are checked regardless of host OS,
since pak archives are portable.

Additional extraction-time protections:

* **Normalized-collision preflight**: before any byte hits disk, pakka
  sorts the selected entries' normalized names (case-folded, with
  `\` mapped to `/` and trailing dot/space stripped) and rejects the
  whole extraction if two entries would materialize to the same path
  on Windows or HFS+. Pre-fix this could silently overwrite files on
  case-insensitive filesystems.
* **Symlink / reparse-point rejection during write**: every directory
  descent into the `-C` destination tree (and every leaf open) uses
  `openat`/`O_NOFOLLOW` on modern POSIX, an `fchdir(O_NOFOLLOW)`
  emulation on legacy POSIX, and a `GetFileAttributesA`-based reparse
  check on Windows. A planted symlink in the destination cannot
  redirect a write outside the requested directory.
* **Symlink rejection on recursive add**: when `-a` recurses into a
  directory, any symlink or reparse point found inside the tree is
  reported and skipped rather than silently followed.

`pakka --verify` runs the same name-safety check and the same
normalized-collision scan without touching disk, so an archive can be
audited before extraction.

## Testing
The repo includes an integration test suite that downloads id's shareware Quake
`pak0.pak` (with a pinned SHA256) and exercises pakka against it: round-trip,
extract-specific, add, delete (including the head, the tail, and every entry),
`--verify`, `--as` aliasing, and rejection of malformed paks.

    $ make test

`make test` runs the full C test suite directly — `symbol-audit`,
`c_api_test`, `dk_codec_test`, and a per-format binary
(`pakka_test`, `pk3_test`, `pk4_test`, `dk_test`, `sin_test`,
`wad_test`, `unicode_paths_test`, `large_file_test`, plus the
realpak suites). No bats, no shell harness; every case is in
`test/*_test.c`.

* `symbol-audit` — runs `nm -g build/lib-test/libpakka.a` and fails
  if any defined global lacks the `pakka_` prefix. Keeps the library
  namespace-clean.
* `c_api_test` — a c-API exerciser linked only against the public
  header. Covers NULL tolerance, round-trips, structured-error
  population, and the `pakka_open_entry` / `pakka_reader_read`
  streaming surface that black-box CLI tests can't reach.

Requires a C99 compiler, `make`, `curl`, `openssl`, and `tar`
(`unzip` only for the optional GoldSrc realpak suites).

### Running tests on Windows

The Windows test path is `cmake -G Ninja -DPAKKA_TEST_BUILD=ON .` +
`ctest` — no bash, no MSYS2. From a stock PowerShell or `cmd.exe`:

    cmake -B build/cmake -G Ninja -DPAKKA_TEST_BUILD=ON .
    cmake --build build/cmake
    pwsh -NoLogo -File dev/win/fixtures.ps1 -Suite quake
    ctest --test-dir build/cmake --output-on-failure

`dev/win/fixtures.ps1` is the PowerShell-native equivalent of `make
fixture` / `make verify-*`. It and the Makefile share fixture URLs
+ SHA-256 pins via `dev/fixtures.mk`.

`make realpak-test-q3` is an opt-in suite that exercises pakka against
a real game-engine archive. It downloads id's Q3 demo wrapper from
archive.org (~93 MiB, SHA256-pinned), uses pakka itself to extract the
inner 47 MiB `pak0.pk3` (1,274 real id-built entries), and runs the
full read / list / extract / structural-verify / deep-verify sequence
against it. `make realpak-test-goldsrc` does the same for Half-Life's
Uplink and Day One PAKs (138 MiB combined). `make realpak-test` is an
umbrella that runs both. Not part of `make test` because of the
download sizes and archive.org's intermittent availability.

On Windows the same coverage is reached through CTest: set the fixture
env var to the path `fixtures.ps1` produces, then filter the CTest run:

    $env:Q3DEMO_PAK0_PK3 = "$pwd\build\test\q3demo\pak0.pk3"
    ctest --test-dir build/cmake -R pk3_q3demo_test --output-on-failure

    $env:GOLDSRC_UPLINK_PAK0 = "$pwd\build\test\hl-uplink\valve\pak0.pak"
    $env:GOLDSRC_DAYONE_PAK0 = "$pwd\build\test\hl-dayone\valve\pak0.pak"
    ctest --test-dir build/cmake -R pak_goldsrc_test --output-on-failure

`make lint` runs [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with
a curated check set (`.clang-tidy` at the repo root). On macOS, install via
`brew install llvm` and either add `$(brew --prefix llvm)/bin` to `PATH` or
invoke as `make CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy lint`. On Linux:
`apt install clang-tidy`.

## Supported Platforms
The code aims to be POSIX-portable across Linux, BSD, and macOS, both
endiannesses, both word sizes. The on-disk pak format is canonically
little-endian; reads and writes go through `pakka_read_u32_le` /
`pakka_write_u32_le` helpers so the same binary works on big-endian
hosts.

CI coverage currently includes:

| OS | Architecture | libc / toolchain | Coverage |
|---|---|---|---|
| macOS 26 | Intel x86_64 | Apple clang | full C suite |
| macOS 26 | Apple Silicon arm64 | Apple clang | full C suite + realpak Q3/GoldSrc |
| Ubuntu latest | x86_64 | glibc / gcc | full C suite + realpak Q3/GoldSrc |
| Ubuntu 24.04 | arm64 | glibc / gcc | full C suite |
| Alpine 3.23 | x86_64 | musl / gcc | full C suite |
| Ubuntu latest | x86_64 (`-m32`) | glibc / gcc (32-bit ABI) | full C suite |
| Debian bookworm-slim (Docker / QEMU) | s390x | glibc / gcc, **big-endian** | full C suite |
| FreeBSD 15.0 | amd64 | clang | full C suite |
| FreeBSD 15.0 | amd64 (`-m32`) | clang (32-bit ABI) | full C suite |
| OpenBSD 7.8 | amd64 | clang | full C suite |
| Windows Server 2025 (VS 2026) | x86_64 | MSVC cl.exe + CMake/Ninja | full C suite + realpak Q3/GoldSrc |
| Debian Sarge-derived (Docker) | i386 | glibc 2.3.2 / gcc 3.3 / GNU make **3.79.1** | build + symlink-safe extract check |
| Red Hat Linux 9 rootfs (Docker) | i386 | glibc 2.3.2 / gcc 3.2.2 / GNU make 3.79.1 | build + symlink-safe extract check |
| NetBSD 3.0 (QEMU) | sparc | BSD libc / gcc 3.3 / GNU make 3.81, **big-endian** | build + symlink-safe extract check |

The `-m32` jobs build 32-bit binaries on a 64-bit kernel/libc — they exercise
the 32-bit code path but aren't a true i386 OS.

The s390x and NetBSD/sparc jobs run under QEMU emulation on x86_64
runners. They are the slowest jobs in the matrix but exist deliberately:
s390x catches byte-order regressions in any new code that touches the
on-disk format, and NetBSD/sparc is the only job that exercises the
pre-openat (`PAKKA_LEGACY_EXTRACT`) BSD code path. Modern BSDs in the
matrix all take the `openat`/`mkdirat` path.

The legacy build jobs run `make` + a banner + symlink-safe extract
check (not the full C test suite — minimal userland on the older
guests can't satisfy every transitive build dependency). The Debian
Sarge and Red Hat Linux 9 jobs keep pakka's **Linux legacy floor**
intact: gcc 3.0+ (first FSF release with adequate C99 support),
glibc 2.2.5+, GNU make 3.79.1+, Linux kernel 2.4+. Concrete distros
that meet that floor with default packages include Red Hat Linux 8.0
(Sept 2002), Red Hat Linux 9 (March 2003), and Debian 3.1 Sarge
(June 2005). The NetBSD/sparc job is a separate class — BSD libc +
big-endian SPARC + the pre-openat BSD branch of `PAKKA_LEGACY_EXTRACT`
(NetBSD < 6.0, FreeBSD < 8.0, OpenBSD < 5.0).

The Windows job builds `pakka.exe` via CMake/MSVC and runs the same
C test binaries via CTest. POSIX builds remain Makefile-driven; CMake
is the Windows path.

## Contributing
1. Fork it!
2. Create your feature branch: `git checkout -b my-new-feature`
3. Commit your changes: `git commit -am 'Add some feature'`
4. Push to the branch: `git push origin my-new-feature`
5. Submit a pull request :D

## History
Created as an excuse to re-learn some C circa December '15–January '16.
Picked up again in 2026 for a substantial cleanup pass — warning-free under
`-Wall --std=c99 --pedantic`, several real bugs fixed, integration test suite
added, and multi-platform CI added.

## Credits
John Carmack for not only creating quake, but then open sourcing it and its
successor games. He is the reason I am a programmer today.

I am unsure who the correct author is, and the original link is now long since
dead, but I followed
[Quake PAK File Format](https://web.archive.org/web/20160711041711/https://debian.fmi.uni-sofia.bg/~sergei/cgsr/docs/pak.txt)
in building this utility.

The [Yamagi Quake II](https://www.yamagi.org/) team's
[pakextract](https://github.com/yquake2/pakextract) (BSD-2-Clause) is
the canonical reference for the SiN and Daikatana on-disk formats —
SiN's SPAK + 120-byte filename layout, Daikatana's 72-byte directory
entry, and especially Daikatana's custom byte-codec compression
(documented opcode-by-opcode at
[`src/dk_codec.c`](src/dk_codec.c)).

## License
MIT
