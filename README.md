# Pakka
A command line utility for working with quake 1 and 2 .pak files.

Why 'pakka', well pak files, and I have kids and Makka Pakka is their favourite
[In the Night Garden](http://www.inthenightgarden.co.uk/) character.

## Disclaimer
I am a novice C programmer. This code is the result of a learning exercise.
Given that C programs can be very brittle, and behave in unexpected ways,
I would strongly suggest you do not use this software :) If you are feeling
very brave and do choose to use this software and find a problem, please let
me know!

## Installation
    $ git clone https://github.com/ajbonner/pakka.git
    $ make

Requires GNU make. On BSD systems, install `gmake` and use `gmake` in place of
`make`.

### Building on Windows (MSVC)

The Makefile is Unix-only. On Windows, build via CMake + `cl.exe` from a
Developer Command Prompt (or after running `vcvarsall.bat x64`):

    > cmake -B build\cmake -G Ninja .
    > cmake --build build\cmake

Produces `build\cmake\pakka.exe`. The CMake build is additive — it compiles
the same `src/*.c` plus the vendored `src/win/wingetopt` (getopt) and
`src/win/dirent` (opendir/readdir) polyfills under `_WIN32`.

## Usage
Pakka has 5 major modes:

* List pak contents: `./pakka -lf <pakfile.pak>`
* Extract: `./pakka -xf <pakfile.pak> [-C <destination>] [path...]`
  * With no path arguments, every entry is extracted.
  * With one or more path arguments, only those entries are extracted; pakka
    errors if any requested path isn't in the pak.
  * `-C` selects a destination directory; defaults to the current working
    directory.
* Create: `./pakka -cf <pakfile.pak> [file/dir...]`
* Add to pak: `./pakka -af <pakfile.pak> [file/dir...]`
* Delete from pak: `./pakka -df <pakfile.pak> [path...]`

`./pakka -h` prints a usage summary.

## Security

Pak entry names are 56 bytes of attacker-controlled data. When extracting,
pakka rejects entry names that would escape the `-C` destination directory:

* empty names
* names beginning with `/` or `\` (absolute paths)
* drive-letter prefixes (`C:...`, `D:...`)
* any path component that is exactly `..` (slash- or backslash-separated)

Substring matches like `foo..bar` or `..png` remain legal — only an exact
`..` component is rejected. Validation runs before any `mkdir`, `fread`, or
`fopen`, so a malicious pak fails fast with no partial writes.

Both POSIX and Windows traversal forms are checked regardless of host OS,
since pak archives are portable.

## Known Limitations

* **Files between 2 GiB and 4 GiB on 32-bit POSIX and Windows MSVC.**
  pakka uses `fseek`/`ftell` (return type `long`), which is 32-bit on
  these targets. The pak format caps offsets at 4 GiB (`u32`), so the
  affected range is 2 GiB to 4 GiB. Behavior is a clean error from the
  bounds validator (rather than silent corruption); 64-bit Linux,
  macOS, and BSD have a 64-bit `long` and aren't affected. Lifting
  the limit would mean migrating to `fseeko`/`ftello` (POSIX) and
  `_fseeki64`/`_ftelli64` (MSVC), plus `-D_FILE_OFFSET_BITS=64` in
  `CPPFLAGS` for 32-bit Linux.

## Testing
The repo includes an integration test suite that downloads id's shareware Quake
`pak0.pak` (with a pinned SHA256) and exercises pakka against it: round-trip,
extract-specific, add, delete (including the head, the tail, and every entry),
and rejection of malformed paks.

    $ make test

Requires [bats-core](https://github.com/bats-core/bats-core)
(`brew install bats-core`, `apt install bats`, `pkg install bats-core`, etc.),
plus `curl`, `openssl`, and `tar` for fetching and verifying the test fixture.

### Running tests on Windows

There's no `make test` one-shot on Windows because the build (MSVC `cl.exe`)
and the test runner (bats under MSYS2 bash) live in different shells. After
building `pakka.exe` via CMake (see Installation), open the **MSYS2 MSYS**
shell, `cd` to the repo, and run:

    $ make fixture                                   # download + SHA-verify pak0.pak
    $ export PAKKA="$PWD/build/cmake/pakka.exe"      # must be a fully-qualified path
    $ bats tests/

**Gotcha:** `PAKKA` has to be **absolute**, not relative. `bats` may `chdir`
into a temp directory inside `setup_file` before invoking pakka, so a
relative path resolves wrong and every test fails to find the binary.

`bats-core` isn't in any MSYS2 repo; install from upstream once:

    $ git clone --depth 1 --branch v1.13.0 \
        https://github.com/bats-core/bats-core.git /tmp/bats-core
    $ /tmp/bats-core/install.sh /usr/local

MSYS2 packages needed alongside bats: `pacman -S --needed bash coreutils curl
diffutils git openssl tar make`.

`make lint` runs [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with
a curated check set (`.clang-tidy` at the repo root). On macOS, install via
`brew install llvm` and either add `$(brew --prefix llvm)/bin` to `PATH` or
invoke as `make CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy lint`. On Linux:
`apt install clang-tidy`.

## Supported Platforms
The code aims to be POSIX-portable across Linux, BSD, and macOS, both
endiannesses, both word sizes. The on-disk pak format is canonically
little-endian; reads and writes go through `read_u32_le` / `write_u32_le`
helpers so the same binary works on big-endian hosts.

CI coverage currently includes:

| OS | Architecture | libc / toolchain | Coverage |
|---|---|---|---|
| macOS 26 | Intel x86_64 | Apple clang | full bats |
| macOS 26 | Apple Silicon arm64 | Apple clang | full bats |
| Ubuntu latest | x86_64 | glibc / gcc | full bats |
| Ubuntu 24.04 | arm64 | glibc / gcc | full bats |
| Alpine 3.23 | x86_64 | musl / gcc | full bats |
| Ubuntu latest | x86_64 (`-m32`) | glibc / gcc (32-bit ABI) | full bats |
| Debian bookworm-slim (Docker / QEMU) | s390x | glibc / gcc, **big-endian** | full bats |
| FreeBSD 15.0 | amd64 | clang | full bats |
| FreeBSD 15.0 | amd64 (`-m32`) | clang (32-bit ABI) | full bats |
| OpenBSD 7.8 | amd64 | clang | full bats |
| Windows Server 2025 (VS 2026) | x86_64 | MSVC cl.exe + CMake/Ninja | full bats |
| Debian Sarge-derived (Docker) | i386 | glibc 2.3.2 / gcc 3.3 / GNU make **3.79.1** | build + symlink-safe extract smoke |
| Red Hat Linux 9 rootfs (Docker) | i386 | glibc 2.3.2 / gcc 3.2.2 / GNU make 3.79.1 | build + symlink-safe extract smoke |
| NetBSD 3.0 (QEMU) | sparc | BSD libc / gcc 3.3 / GNU make 3.81, **big-endian** | build + symlink-safe extract smoke |

The `-m32` jobs build 32-bit binaries on a 64-bit kernel/libc — they exercise
the 32-bit code path but aren't a true i386 OS.

The s390x and NetBSD/sparc jobs run under QEMU emulation on x86_64
runners. They are the slowest jobs in the matrix but exist deliberately:
s390x catches byte-order regressions in any new code that touches the
on-disk format, and NetBSD/sparc is the only job that exercises the
pre-openat (`PAKKA_LEGACY_EXTRACT`) BSD code path. Modern BSDs in the
matrix all take the `openat`/`mkdirat` path.

The legacy smoke jobs run `make` + a banner + symlink-safe extract
smoke (not the full bats suite — bash 2.05b / minimal userland on
the older guests). The Debian Sarge and Red Hat Linux 9 jobs keep
pakka's **Linux legacy floor** intact: gcc 3.0+ (first FSF release with
adequate C99 support), glibc 2.2.5+, GNU make 3.79.1+, Linux kernel
2.4+. Concrete distros that meet that floor with default packages
include Red Hat Linux 8.0 (Sept 2002), Red Hat Linux 9 (March 2003),
and Debian 3.1 Sarge (June 2005). The NetBSD/sparc job is a separate
class — BSD libc + big-endian SPARC + the pre-openat BSD branch of
`PAKKA_LEGACY_EXTRACT` (NetBSD < 6.0, FreeBSD < 8.0, OpenBSD < 5.0).

The Windows job builds `pakka.exe` via CMake/MSVC and runs the bats suite
through MSYS2 bash. POSIX builds remain Makefile-driven; CMake is the
Windows-only path.

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

## License
MIT
