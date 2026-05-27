# Installation

The README has the quickstart. This document covers the install
layout, `PREFIX` / `DESTDIR` overrides, and the Windows CMake / MSVC
build.

## Release artifacts

Each tagged release attaches the following to the
[GitHub release](https://github.com/ajbonner/pakka/releases), each
with a `.sha256` sidecar:

| Artifact                                    | Built on                              |
| ------------------------------------------- | ------------------------------------- |
| `pakka-X.Y.Z.tar.gz` — source tarball       | Any POSIX with GNU make + C99         |
| `pakka-X.Y.Z-windows-x64.zip`               | Windows 64-bit (MSVC + CMake)         |
| `pakka-X.Y.Z-windows-x86.zip`               | Windows 32-bit (MSVC + CMake)         |
| `libpakka-X.Y.Z-windows-x64.zip`            | Windows 64-bit, library-only (no CLI) |
| `libpakka-X.Y.Z-windows-x86.zip`            | Windows 32-bit, library-only (no CLI) |
| `pakka-X.Y.Z-macos-x86_64.tar.gz`           | macOS Intel                           |
| `pakka-X.Y.Z-macos-arm64.tar.gz`            | macOS Apple Silicon                   |

The macOS and Windows packages are built on native runners — no
cross-compile, no universal-binary merge. The `libpakka-*` zips are the
same Windows build with the CLI omitted — static lib, header, and
`pakka(3)` manpage only — for integrators embedding libpakka.

## From the source tarball (Unix)

Each tagged release attaches a `pakka-X.Y.Z.tar.gz` source tarball
and a SHA-256 sidecar to the
[GitHub release](https://github.com/ajbonner/pakka/releases). Grab
them, verify the checksum, build, and install:

    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.8.0/pakka-1.8.0.tar.gz
    $ curl -fLO https://github.com/ajbonner/pakka/releases/download/v1.8.0/pakka-1.8.0.tar.gz.sha256
    $ shasum -a 256 -c pakka-1.8.0.tar.gz.sha256
    $ tar xzf pakka-1.8.0.tar.gz
    $ cd pakka-1.8.0
    $ make

Then pick one install flavor:

    $ sudo make install                    # system-wide to /usr/local
    $ make install PREFIX=$HOME/.local     # user-only, no root
    $ make install DESTDIR=/tmp/stage      # packager staging (.deb / .rpm / pkg)

`PREFIX` defaults to `/usr/local`, which is root-owned on most
systems — plain `make install` will fail with "permission denied"
without `sudo` or a user-writable `PREFIX`. `make uninstall` reverses
any of the above (pass the same `PREFIX` / `DESTDIR` you installed
with).

Requires GNU make and a C99 compiler. On BSD systems install `gmake`
and use it in place of `make`. The build is dependency-free at
runtime — the DEFLATE codec is bundled.

## Installed layout (Unix)

GNU install conventions, via the GNU install dirs:

| File                        | Path                                     |
| --------------------------- | ---------------------------------------- |
| `pakka` (CLI)               | `$(PREFIX)/bin/pakka`                    |
| `libpakka.a` (static lib)   | `$(PREFIX)/lib/libpakka.a`               |
| `pakka.h` (public header)   | `$(PREFIX)/include/pakka.h`              |
| `pakka(1)` (CLI manpage)    | `$(PREFIX)/share/man/man1/pakka.1`       |
| `pakka(3)` (libpakka manpage) | `$(PREFIX)/share/man/man3/pakka.3`     |
| README + LICENSE            | `$(PREFIX)/share/doc/pakka/`             |

## Windows zip layout

Releases ship `pakka-X.Y.Z-windows-x64.zip` and
`pakka-X.Y.Z-windows-x86.zip`, both produced via `cpack -G ZIP`
against the MSVC build. Layout (x64 shown; x86 is identical with the
`-x64` suffix swapped for `-x86`):

    pakka-1.8.0-windows-x64\
      bin\pakka.exe
      lib\pakka.lib                       # static lib for integrators
      include\pakka.h                     # public C header
      share\man\man1\pakka.1
      share\man\man3\pakka.3
      share\doc\pakka\README.md
      share\doc\pakka\LICENSE

The library-only `libpakka-X.Y.Z-windows-x64.zip` and
`libpakka-X.Y.Z-windows-x86.zip` are the same build configured with
`PAKKA_BUILD_CLI=OFF` — no `pakka.exe`, no `pakka(1)` manpage. For C/C++
integrators embedding libpakka who don't need the CLI:

    libpakka-1.8.0-windows-x64\
      lib\pakka.lib                       # static library
      include\pakka.h                     # public C header
      share\man\man3\pakka.3
      share\doc\pakka\README.md
      share\doc\pakka\LICENSE

## macOS archive layout

Releases also ship native macOS binary archives — a `.tar.gz` for Intel
(`x86_64`) and Apple Silicon (`arm64`), produced via `make install` into
a staged package root. Layout (Apple Silicon shown):

    pakka-1.8.0-macos-arm64/
      bin/pakka
      lib/libpakka.a
      include/pakka.h
      share/man/man1/pakka.1
      share/man/man3/pakka.3
      share/doc/pakka/README.md
      share/doc/pakka/LICENSE

## From source (git clone)

For contributors:

    $ git clone https://github.com/ajbonner/pakka.git
    $ cd pakka
    $ make

`make` produces two artifacts:

* `./pakka` — the command-line binary documented in the README.
* `build/lib-prod/libpakka.a` — a static library exposing pakka's
  archive operations as a C99 API. The public header is at
  `include/pakka.h`; every exported symbol is prefixed `pakka_` and
  `make symbol-audit` fails the build if anything else leaks out.

`make dist` produces the release tarball locally
(`pakka-X.Y.Z.tar.gz` + SHA-256 sidecar) via `git archive HEAD`.

## Building on Windows (MSVC)

The Makefile is Unix-only. On Windows, build via CMake + `cl.exe`
from a Developer Command Prompt (or after running
`vcvarsall.bat x64`):

    > cmake -B build\cmake -G Ninja .
    > cmake --build build\cmake

Produces `build\cmake\pakka.exe`. The CMake build compiles the same
`src/*.c` (including `src/pk3file.c` and the vendored
`src/vendor/sdefl` / `src/vendor/sinfl` DEFLATE codec, or `<zlib.h>`
with `-DPAKKA_USE_ZLIB=ON`) plus the `src/vendor/wingetopt` (getopt)
and `src/vendor/dirent` (opendir / readdir) polyfills under
`_WIN32`.

Supported Windows floor is **Windows XP SP3**, built with the VS
2017 `v141_xp` toolset for releases targeting XP / Vista / 7. CI
uses VS 2026 on Windows Server 2025. The source intentionally avoids
API calls that require later Windows versions so the same tree
builds against either toolset.

## DEFLATE codec backend

The DEFLATE codec is selectable at build time. Default is the
bundled `sdefl` + `sinfl` single-header encoder / decoder pair (no
external dependency, ~1400 LOC vendored under `src/vendor/`):

    $ make                            # default: bundled codec
    > cmake -B build/cmake .          # same on Windows

Integrators who already link `zlib` can route DEFLATE through it
instead and skip the duplicate codec code:

    $ make PAKKA_DEFLATE_BACKEND=zlib
    > cmake -B build/cmake -DPAKKA_USE_ZLIB=ON .

Both backends produce byte-interchangeable raw RFC 1951 DEFLATE
streams.
