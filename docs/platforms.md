# Supported platforms

Pakka aims to be POSIX-portable across Linux, BSD, and macOS — both
endiannesses, both word sizes — and to build on Windows via CMake +
MSVC. The on-disk pak format is canonically little-endian; reads and
writes go through `pakka_read_u32_le` / `pakka_write_u32_le` so the
same binary works on big-endian hosts.

## CI matrix

| OS                                | Architecture           | libc / toolchain                          | Coverage                              |
| --------------------------------- | ---------------------- | ----------------------------------------- | ------------------------------------- |
| macOS 26                          | Intel x86_64           | Apple clang                               | full C suite                          |
| macOS 26                          | Apple Silicon arm64    | Apple clang                               | full C suite + realpak Q3/GoldSrc     |
| Ubuntu latest                     | x86_64                 | glibc / gcc                               | full C suite + realpak Q3/GoldSrc     |
| Ubuntu 24.04                      | arm64                  | glibc / gcc                               | full C suite                          |
| Alpine 3.23                       | x86_64                 | musl / gcc                                | full C suite                          |
| Ubuntu latest                     | x86_64 (`-m32`)        | glibc / gcc (32-bit ABI)                  | full C suite                          |
| Debian bookworm-slim (Docker/QEMU)| s390x                  | glibc / gcc, **big-endian**               | full C suite                          |
| FreeBSD 15.0                      | amd64                  | clang                                     | full C suite                          |
| FreeBSD 15.0                      | amd64 (`-m32`)         | clang (32-bit ABI)                        | full C suite                          |
| OpenBSD 7.8                       | amd64                  | clang                                     | full C suite                          |
| Windows Server 2025 (VS 2026)     | x86_64                 | MSVC `cl.exe` + CMake/Ninja               | full C suite + realpak Q3/GoldSrc     |
| Debian Sarge-derived (Docker)     | i386                   | glibc 2.3.2 / gcc 3.3 / GNU make 3.79.1   | build + symlink-safe extract check    |
| Red Hat Linux 9 rootfs (Docker)   | i386                   | glibc 2.3.2 / gcc 3.2.2 / GNU make 3.79.1 | build + symlink-safe extract check    |
| NetBSD 3.0 (QEMU)                 | sparc                  | BSD libc / gcc 3.3 / GNU make 3.81, **big-endian** | build + symlink-safe extract check |

The `-m32` jobs build 32-bit binaries on a 64-bit kernel and libc —
they exercise the 32-bit code path but aren't a true i386 OS.

The s390x and NetBSD/sparc jobs run under QEMU emulation on x86_64
runners. They are the slowest jobs in the matrix but exist
deliberately: s390x catches byte-order regressions in any new code
that touches the on-disk format, and NetBSD/sparc is the only job
that exercises the pre-`openat` (`PAKKA_LEGACY_EXTRACT`) BSD code
path. Modern BSDs in the matrix all take the `openat` / `mkdirat`
path.

## Legacy floors

The legacy build jobs run `make` plus a banner and a symlink-safe
extract check (not the full C test suite — minimal userland on the
older guests can't satisfy every transitive build dependency).

**Linux legacy floor (design target).** The pakka source is written to
build against:

* gcc 3.0+ (first FSF release with adequate C99 support)
* glibc 2.2.5+
* GNU make 3.79.1+
* Linux kernel 2.4+

Concrete distros that meet that target with default packages include
Red Hat Linux 8.0 (Sept 2002), Red Hat Linux 9 (March 2003), and
Debian 3.1 Sarge (June 2005).

**What CI actually verifies.** The Docker legacy jobs run on the
modern GitHub-host kernel and exercise toolchains close to — but not
exactly at — the floor:

* `legacy-glibc` — Sarge-derived userland: glibc 2.3.2, gcc 3.3, GNU
  make 3.79.1 built from source. Linux/386.
* `legacy-rh9` — literal Red Hat 9 toolchain assembled from
  SHA-pinned vault RPMs: glibc 2.3.2-11.9, gcc 3.2.2-5, GNU make
  3.79.1-17. Linux/386.

These catch post-2006 POSIX dependencies and make-3.80+ syntax
regressions before they slip past the modern matrix. The kernel-2.4
and glibc-2.2.5 lower bounds are design intent, not CI-verified —
the QEMU probe in `dev/legacy/rh9/` is the fallback for verifying
against the literal kernel/glibc floor when needed.

**BSD legacy floor.** The NetBSD/sparc job is a separate class — BSD
libc plus big-endian SPARC plus the pre-`openat` BSD branch of
`PAKKA_LEGACY_EXTRACT`. That branch targets NetBSD < 6.0, FreeBSD <
8.0, and OpenBSD < 5.0. CI boots a literal NetBSD 3.0/sparc guest
under QEMU and builds + tests over SSH.

## Additional CI jobs

Beyond the platform matrix above, CI runs cross-cutting jobs that
exercise build variants and analysis tools:

| Job                      | What it does                                                                                       |
| ------------------------ | -------------------------------------------------------------------------------------------------- |
| `lint`                   | clang-tidy with the curated `.clang-tidy` check set against the POSIX source.                      |
| `lint-win32`             | clang-tidy under `--target=x86_64-w64-mingw32` so the `_WIN32` arm gets linted without MSVC.       |
| `linux-glibc-x86_64-zlib`| Full C suite with `PAKKA_DEFLATE_BACKEND=zlib` — verifies the system-zlib codec path stays sound.  |
| `asan-ubsan-x86_64`      | Full C suite under `-fsanitize=address,undefined` with `-fno-sanitize-recover=undefined`.          |
| `fuzz`                   | libFuzzer harnesses for `pakka_open`, `pakka_dk_inflate`, and open→commit→reopen roundtrip (60s each). Push / schedule only. |
| `coverage`               | lcov HTML report from the full C suite. Push / schedule only.                                      |
| `realpak-test-*`         | Q3 demo and Half-Life Uplink / Day One realpak suites on Linux, macOS, and Windows. See [`testing.md`](testing.md). |

## Build paths

POSIX builds are Makefile-driven. The Windows job builds `pakka.exe`
via CMake / MSVC and runs the same C test binaries through CTest —
CMake is the canonical Windows path.

See [`install.md`](install.md) for build commands on each platform.
