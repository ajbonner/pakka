# Pakka
A C library (`libpakka`) for reading, writing, and editing the archive
formats used by id Software, Ritual, Ion Storm, and Valve game engines
— Quake 1 / 2 and Half-Life / GoldSrc PAK, SiN SPAK, Daikatana PAK,
Doom IWAD / PWAD, Quake 3 PK3, and Doom 3 PK4. The `pakka(1)`
command-line tool is a thin wrapper around the same API.

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden](http://www.inthenightgarden.co.uk/) character.

Full documentation is published at
**[ajbonner.github.io/pakka](https://ajbonner.github.io/pakka/)**.

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

    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.6.0/pakka-1.6.0.tar.gz
    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.6.0/pakka-1.6.0.tar.gz.sha256
    $ shasum -a 256 -c pakka-1.6.0.tar.gz.sha256
    $ tar xzf pakka-1.6.0.tar.gz
    $ cd pakka-1.6.0
    $ make
    $ sudo make install                    # or PREFIX=$HOME/.local for user-only

Requires GNU make and a C99 compiler. On BSD systems install `gmake` and
use it in place of `make`. The build is dependency-free at runtime — the
DEFLATE codec is bundled.

See [Installation docs](https://ajbonner.github.io/pakka/install/) for
`PREFIX` / `DESTDIR` overrides, the installed file layout, and the
`zlib` backend opt-in.

### From a pre-built binary

Releases ship pre-built archives alongside the source tarball:

* `pakka-X.Y.Z-windows-x64.zip` and `pakka-X.Y.Z-windows-x86.zip`
* `pakka-X.Y.Z-macos-x86_64.tar.gz` / `.zip` (Intel)
* `pakka-X.Y.Z-macos-arm64.tar.gz` / `.zip` (Apple Silicon)

Each carries a `.sha256` sidecar. Unzip / untar anywhere and add the
extracted `bin/` to your `PATH`, or copy the `pakka` binary into a
directory already on `PATH`.

### From source (git clone)

    $ git clone https://github.com/ajbonner/pakka.git
    $ cd pakka
    $ make

See [Installation docs](https://ajbonner.github.io/pakka/install/) for
the CMake / MSVC Windows build.

## Usage

The archive path is the first positional argument; entry names or
source files follow. Option flags must come before the archive —
pakka's getopt is POSIX-strict and stops scanning at the first
positional.

* `pakka -l <archive>` — list contents. Add `--tree` for a UTF-8
  directory tree.
* `pakka -x [-C <dest>] <archive> [path...]` — extract. With no paths,
  extracts everything; with paths, errors if any aren't in the archive.
* `pakka -c <archive> [file/dir...]` — create. Format follows the
  destination extension; `--format <name>` overrides it.
* `pakka -a <archive> [file/dir...]` — add. Use `--as <entry-name>
  <source>` to add a single file under an explicit entry name.
* `pakka -d <archive> [path...]` — delete entries.
* `pakka --verify [--deep] <archive>` — walk every entry, run
  name-safety and collision checks, confirm the directory points at
  readable bytes. `--deep` adds per-entry CRC32 / decompression
  checks. Exits non-zero on any error-level finding.

`pakka -h` for the full flag list; `pakka -V` for version and
supported formats; `man pakka` for the manpage.

## Format support

Pakka reads, writes, and edits every supported format with the full
operation set — list, extract, create, add, delete, verify — across:

* Quake 1 / 2 PAK and Half-Life / GoldSrc PAK (bit-identical layout)
* SiN
* Daikatana
* Doom IWAD and PWAD
* Quake 3 PK3
* Doom 3 PK4

PK3 / PK4 entries are stored uncompressed by default (the ZIP "STORED"
method — raw bytes, no codec). Pass `--compress` (or
`pakka_set_compression(archive, PAKKA_COMPRESSION_DEFLATE, ...)` from
the library) to DEFLATE-encode new entries instead; pakka falls back
to uncompressed per-entry when DEFLATE wouldn't shrink the input.

Daikatana is the odd one out — it shares Quake's `"PACK"` magic but
widens the directory entry to add a custom LZSS-style byte-codec for a
handful of asset types. Pakka probes both layouts at open time and
applies the original Ion Storm packer's add-time policy (`.tga`,
`.bmp`, `.wal`, `.pcx`, `.bsp` are compressed; everything else is
stored uncompressed). Pin the format explicitly with `--format
daikatana` if the on-disk geometry is ambiguous.

Doom WAD support covers both IWAD (id-shipped base archives) and PWAD
(modder patches) — `.wad` defaults to PWAD on create; pass `--format
iwad` for the base flavour.

See [Format support docs](https://ajbonner.github.io/pakka/formats/)
for the compression policies, the Daikatana codec details, the Doom
WAD lump-naming rules, and the ZIP features pakka refuses on read
(ZIP64, encryption, multi-disk, unsupported compression methods).

## Using libpakka from C

`include/pakka.h` exposes an opaque-handle C99 API for opening,
listing, extracting from, and mutating pak archives. Every exported
symbol is prefixed `pakka_`, library functions never call `exit` or
write to stdout / stderr, and every failure returns a `pakka_status_t`
and optionally populates a caller-provided `pakka_error_t` with
structured detail.

`test/c_api_test.c` is the canonical call-pattern example, linking
only against the public header.

See [libpakka docs](https://ajbonner.github.io/pakka/libpakka/) for
the function inventory grouped by responsibility, or `man 3 pakka`
for the full reference (`man/pakka.3` in this tree, or
`${MANDIR}/man3/pakka.3` after `make install`).

## Security

Pak entry names are attacker-controlled data. Extract, add, and verify
share a fail-fast validator (`pakka_unsafe_entry_name`) that rejects:

* path traversal — absolute paths, `..` components, drive letters, UNC
* Windows-hostile names — control bytes, colons, trailing dot / space,
  reserved devices (`CON` / `NUL` / `COM0`–`COM9` / `LPT0`–`LPT9`)

Plus three extraction-time protections:

* **Normalized-collision preflight** — refuses extractions where two
  entries would materialize to the same path on case-insensitive
  filesystems (Windows, HFS+).
* **Symlink / reparse-point rejection during write** — every directory
  descent uses `O_NOFOLLOW`, an `fchdir` emulation on legacy POSIX, or
  a `GetFileAttributesA` check on Windows, so a planted symlink in the
  destination can't redirect a write outside it.
* **Symlink rejection on recursive add** — symlinks inside a tree
  passed to `-a` are reported and skipped, not silently followed.

`pakka --verify` runs every disk-free check without extracting, so an
archive can be audited before use.

See [Security docs](https://ajbonner.github.io/pakka/security/) for
the full byte-level rejection rules and the decompression-bomb cap.

## Supported platforms

POSIX-portable across Linux, BSD, and macOS — both endiannesses, both
word sizes — plus Windows via CMake / MSVC. CI exercises the C suite
on macOS (Intel + arm64), Ubuntu (x86_64 + arm64 + 32-bit), Alpine
musl, FreeBSD, OpenBSD, Debian s390x (big-endian), and Windows Server
2025 + VS 2026. Sanitizers (ASan + UBSan), libFuzzer harnesses, and a
`zlib`-backend variant run alongside.

The legacy design target is Linux kernel 2.4 / glibc 2.2.5 / gcc 3.0 /
make 3.79.1 (Red Hat 9 era); Docker jobs verify against glibc 2.3.2 +
gcc 3.2.2 / 3.3, and a QEMU NetBSD/sparc job covers the pre-`openat`
BSD path.

See [Platform docs](https://ajbonner.github.io/pakka/platforms/) for
the full CI matrix and legacy-floor details.

## Testing

    $ make test

Builds and runs the full C test suite — round-trip, extract-specific,
add, delete (head / tail / every entry), `--verify`, `--as` aliasing,
malformed-pak rejection — across every supported format. The Quake
fixture (id's shareware `pak0.pak`) downloads once with a pinned
SHA-256.

Requires a C99 compiler, `make`, `curl`, `openssl`, and `tar`.

See [Testing docs](https://ajbonner.github.io/pakka/testing/) for the
Windows test path (CMake + CTest), the realpak opt-in suites (Q3
demo, Half-Life Uplink and Day One), and `make lint`.

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
