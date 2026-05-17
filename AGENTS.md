# AGENTS.md

Guidance for AI coding agents working in this repository. This file is derived
from `CLAUDE.md`; keep both files in sync when repository guidance changes.

## Commands

- `make` builds `./pakka` (GNU make required; on BSD use `gmake`).
- Windows (MSVC): `cmake -B build/cmake -G Ninja . && cmake --build build/cmake`
  produces `build/cmake/pakka.exe`. The Makefile is Unix-only; `CMakeLists.txt`
  is purely additive and does not replace it. `/W4 /WX` mirrors the Unix
  `WarningsAsErrors: '*'` policy.
- `make fixture` downloads and SHA-verifies `pak0.pak` only (no build). This lets
  the Windows CI job fetch the bats fixture without using the Makefile build
  pipeline.
- `make test` runs the bats integration suite. It downloads id's shareware Quake
  tarball to `build/test/quakesw.tar.gz` and verifies its SHA256 against
  `QUAKE_SHA256` in the Makefile before extracting `pak0.pak`. It needs
  `bats-core`, `curl`, `openssl`, and `tar` on `PATH`.
- Windows tests: there is no `make test` one-shot. Build the exe via CMake first,
  then from MSYS2 bash at the repo root run
  `make fixture && export PAKKA="$PWD/build/cmake/pakka.exe" && bats tests/`.
  `PAKKA` must be a fully-qualified path, not relative; bats may `chdir` inside
  `setup_file` (commit `6d54723`), and a relative `PAKKA` resolves against the
  wrong cwd. `bats-core` has to be installed from upstream into `/usr/local`
  because MSYS2 has no package; see README "Running tests on Windows".
- `make lint` runs clang-tidy with the curated check set in `.clang-tidy`
  (`WarningsAsErrors: '*'`). On macOS, install via `brew install llvm` and either
  prepend `$(brew --prefix llvm)/bin` to `PATH` or invoke as
  `make CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy lint`; Apple's bundled
  toolchain does not ship clang-tidy.
- `make clean` removes objects and the binary; `make test-clean` removes test
  scratch directories; `make distclean` wipes all of `build/`.
- Run a single bats test by name:
  `bats tests/pakka.bats --filter 'duplicate path'` after `make` and
  `make $(PAK0)` if `pak0.pak` is not already in `build/test/`.
- 32-bit build, mirroring the CI `-m32` jobs:
  `make CFLAGS="-g -Wall --std=c99 --pedantic -m32"` (needs `gcc-multilib` on
  Debian/Ubuntu).
- Legacy QEMU build probe: Red Hat Linux 9.0 "Shrike" (early-2000s target,
  released March 31, 2003; not RHEL 9), kernel 2.4.20-8, glibc 2.3.2, GNU make
  3.79.1. See `dev/legacy/rh9/README.md`.

CI (`.github/workflows/test.yml`) runs the bats suite on macOS Intel + Apple
Silicon, Ubuntu glibc x86_64/arm64, Alpine musl, Ubuntu `-m32`, FreeBSD 15 amd64
plus `-m32`, OpenBSD 7.8, and Windows Server 2025 / MSVC x86_64. POSIX targets
go through `make test`; the Windows job builds via CMake + Ninja + `cl.exe` and
runs bats through MSYS2 bash (`update: false` on `setup-msys2` to keep its cache
hot; `bats-core` is cloned from upstream because MSYS2 has no package; `PAKKA`
is exported as an MSYS2-style absolute path because bats's `setup_file` can
`chdir`). Keep new code POSIX-portable and 32-or-64-bit safe, route on-disk u32
reads/writes through `read_u32_le`/`write_u32_le` (the file format is LE
regardless of host endianness), and do not add direct `<unistd.h>`,
`<dirent.h>`, `realpath`, or `getopt` calls without going through
`src/compat.h`.

## Architecture

`pakka` is a single-binary CLI with five mutually exclusive modes (`-l` list,
`-x` extract, `-c` create, `-a` add, `-d` delete) selected in `src/main.c`. All
real work happens in `src/pakfile.c`.

### Pak File Layout (Quake 1/2)

The pak header is 12 bytes: `"PACK"` magic + `diroffset` (u32 LE) + `dirlength`
(u32 LE). The directory is `dirlength / 64` entries, each 64 bytes: 56-byte
filename + u32 offset + u32 length. The directory can sit anywhere in the file;
id's `pak0.pak` famously has it mid-file with a 410,616-byte orphan `progs.dat`
in a hole left by an in-place recompile. Do not write code that assumes
directory order equals byte layout order.

Every on-disk u32 (`diroffset`, `dirlength`, per-entry `offset`/`length`) is
stored **little-endian regardless of host byte order**. New code that touches
the on-disk format must go through `read_u32_le` / `write_u32_le` in
`src/common.h`, never a raw `fread`/`fwrite` of a `uint32_t`. The
`linux-glibc-s390x-be` CI job runs the bats suite under big-endian s390x via
QEMU and exists to catch byte-order regressions in this code path.

### Module Map

- `src/main.c`: arg parsing (`getopt`, `-lxcadhf:C:`), dispatch, and the
  `extract()` wrapper that resolves the `-C` destination through
  `compat_realpath` / `compat_getcwd` before handing off.
- `src/compat.{h,c}`: POSIX/Win32 shim layer. On POSIX every `compat_*` is a
  one-line forwarder; on Windows it routes to `_fullpath`, `_getcwd`, `_mkdir`,
  `_strdup`, `GetTempPathA` + `_mktemp_s`, `MoveFileExA`, and a custom
  `compat_dirname` that handles both `/` and `\` separators with POSIX
  trailing-separator semantics. `compat_mkstemp_open` consolidates POSIX
  `mkstemp` + `fdopen` and Windows `GetTempPathA` + `_mktemp_s` + `fopen("wb")`
  into one call, and removes the hard-coded `/tmp/` assumption. `compat_rename_replace`
  uses `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)` on Windows; see the
  rename-while-open rule under Conventions.
- `src/win/`: vendored polyfills, compiled only when `_WIN32` is defined.
  `wingetopt/` (alex85k/wingetopt v1.00 @ effe6e2, ISC) supplies `getopt()`;
  `dirent/` (tronkko/dirent v1.26 @ 31db647, MIT) supplies `<dirent.h>` for the
  recursive-add path. Pinned tags and commit SHAs live in `src/win/VENDOR.md`;
  update those values when updating the vendored code.
- `src/pakfile.c`: opens/creates the pak, loads the directory into a singly
  linked list of `Pakfileentry_t` rooted at `Pak_t.head`, and implements
  list/extract/add/delete/write-directory. It holds file-local globals (`fp`,
  `cur_pakpath`, `new_pakpath`, `tmp_pakpath`, `is_new`); `Pak_t` is not
  self-contained because the open file handle lives in this translation unit.
  Only one pak can be open per process.
- `src/filesystem.c`: `mkdir_r` (recursive mkdir, mutates the path string in
  place), `file_exists`, and `filesize`.
- `src/common.c`: `error_exit(fmt, ...)` is the standard failure path.
  Intentionally not marked `noreturn` (see `.clang-tidy` header), which is why
  `clang-analyzer-*` is disabled there.
- `src/debug.c`: `debug_header` / `debug_directory_entry`. Called under
  `#ifndef _DEBUG`, but the Makefile defines `_DEBUG=1` in `CPPFLAGS`, so these
  calls are compiled out of normal builds. The polarity is inverted; flipping it
  would be a behavior change.

### Write Strategies

- Create/add (`add_files` -> `add_file`/`add_folder` -> `write_pak_directory`):
  writes directly to the destination file (`r+` mode for add, or an `mkstemp`
  that gets renamed on close for create). Appends payload bytes after the current
  tail, then rewrites the directory and header in place.
- Delete (`delete_entries`): always rebuilds via a fresh `mkstemp` tempfile,
  copies retained entries through `copy_between_paks`, writes a new directory,
  and renames over the original. This makes delete safe against id's
  non-sequential `pak0.pak` layout; do not "optimize" it into an in-place shift.

`write_pak_directory` handles the empty-pak case explicitly: a pak with every
entry deleted is rewritten as a bare 12-byte `PACK\0\0\0\f\0\0\0\0` header
rather than left malformed. The `delete: removing every entry` bats test pins
this.

## Conventions

- Errors abort the process via `error_exit`; there is no recovery/cleanup path.
  Memory leaks on the error path are acceptable because the OS reclaims at exit.
- `Pak_t.head` may be `NULL` for an empty pak; check before iterating.
  `find_entry`, `list_files`, `extract_files`, `delete_entries`, and
  `write_pak_directory` all handle this case.
- Use C99 + `--pedantic` + `-D_POSIX_C_SOURCE=200809L`. `dirname()` from
  `<libgen.h>` is allowed to mutate its argument and/or return a static buffer;
  always pass it a `strdup`'d copy you own (see `extract_files`). Use
  `compat_dirname` from `src/compat.h`, not `<libgen.h>` directly, so Windows
  builds get the bundled fallback.
- `extract_files` must not break out of its match loop on first hit. Duplicate
  path arguments need every `path_matched[i]` flagged, otherwise the trailing
  completeness check spuriously errors. A regression test covers this.
- Path-traversal rejection on extract: `is_unsafe_extract_path`
  (`src/pakfile.c:649`) runs at the top of the `extract_files` loop (called at
  `src/pakfile.c:459`) before any `mkdir`/`fread`/`fopen`. It rejects empty
  names, leading `/` or `\`, drive-letter prefixes (`X:`), and any exact `..`
  path component (slash- or backslash-separated). Substring patterns such as
  `..foo` or `foo..bar` stay legal. Both POSIX and Windows traversal forms are
  checked on every host because paks are portable. Test coverage is in the
  `extract: refuses ...` cases in `tests/pakka.bats`. Do not relax this in a
  "host does not care about backslash" optimization; a Linux user extracting an
  attacker-supplied pak still has to be safe.
- `fopen` binary modes are mandatory: every pak/payload open uses `"rb"`,
  `"wb"`, or `"r+b"`. On POSIX the `b` is a no-op; on Windows it suppresses
  text-mode `\n` <-> `\r\n` translation that would silently corrupt extracted
  `.pak`/`.mdl`/`.bsp`/`.wav` payloads. Never write a plain `"r"`, `"w"`, or
  `"r+"` against a pak or its contents.
- Close before rename: POSIX allows renaming over an open file; Windows CRT
  `fopen` omits `FILE_SHARE_DELETE`, so `MoveFileExA` is blocked while the
  `FILE*` is live. Both rename sites (`close_pakfile` in `src/pakfile.c` and
  `delete_entries` in `src/pakfile.c`) `fclose(fp)` and null it out before
  calling `compat_rename_replace`. `close_pakfile` tolerates `fp == NULL` for
  this reason. Do not reorder.
- Bats CRLF tolerance: MSVC-built `pakka.exe` produces CRLF on stdout in text
  mode; POSIX builds produce LF. The `--tree` assertions in `tests/pakka.bats`
  strip `\r` before exact-string comparison (`"${output//$'\r'/}"`). New tests
  that compare exact stdout against a multi-line expected string should do the
  same.
- Large-file ceiling on 32-bit POSIX and Windows MSVC: `fseek`/`ftell` use
  `long`, which is 32-bit on `-m32` and on LLP64 Windows. Files between 2 GiB
  and 4 GiB therefore fail the bounds validator with a clean error rather than
  corrupting; the pak format itself caps at 4 GiB (`u32`). 64-bit Linux/macOS/BSD
  have 64-bit `long` and are not affected. Lifting the limit means switching to
  `fseeko`/`ftello` (POSIX) + `_fseeki64`/`_ftelli64` (MSVC) and adding
  `-D_FILE_OFFSET_BITS=64` to `CPPFLAGS` for 32-bit Linux. Codex flagged this in
  the Wave 4 review; deferred because the affected range is narrow and the
  failure mode is safe.
- Legacy floor: gcc 3.0+ (first FSF release supporting the C99 subset pakka
  uses under `-std=c99`: mixed declarations and `<stdint.h>` exact-width types),
  glibc 2.2.5+, GNU make 3.79.1+, Linux kernel 2.4+. Concrete distros that meet
  the floor with default packages: Red Hat Linux 8.0 (Sept 2002, gcc 3.2),
  Red Hat Linux 9 (March 2003, gcc 3.2.2), Debian 3.1 Sarge (June 2005, gcc
  3.3). Default compilers on Red Hat 7.0-7.3 (gcc 2.96) and Debian Woody (gcc
  2.95.4) are not supported; their C99 implementations are incomplete. The
  Makefile avoids order-only prerequisites; `src/compat.c` swaps the
  openat-based extract path for an `fchdir` + `O_NOFOLLOW` emulation when
  `!__GLIBC_PREREQ(2,4)`, keeping the same symlink-rejection guarantee. CI
  verifies via two images in `dev/legacy/ci/` (Sarge-derived + custom make
  3.79.1, and an RH9-rootfs image built from SHA-pinned vault RPMs); the QEMU
  probe in `dev/legacy/rh9/` remains the authoritative real-hardware verifier.
- Deferred legacy targets (not yet in CI, intentionally): the historical
  default-toolchain BSDs (FreeBSD 4.11, OpenBSD 3.3, NetBSD 1.6 sparc/macppc)
  all ship gcc 2.95, which is below the documented C99 floor. Adding them
  means either installing gcc 3.x in each guest from period-correct
  ports/pkgsrc (brittle, since those archives have rotated), or lowering the
  floor to gcc 2.95 (effectively a C90 migration). They also predate
  `openat`/`mkdirat` in their respective libcs (FreeBSD 8.0, OpenBSD 5.0,
  NetBSD 6.0), so `PAKKA_LEGACY_EXTRACT` would need to recognize old BSD
  version macros, not just `__GLIBC_PREREQ(2,4)`. Each target is QEMU-disk-
  image infrastructure on the scale of `dev/legacy/rh9/`; add them one by one
  if the demand justifies the cost. Alpha is also skipped: it is little-endian
  in every shipped configuration (FreeBSD/NetBSD/Linux), so it adds no
  byte-order coverage beyond what `linux-glibc-s390x-be` already gives.
