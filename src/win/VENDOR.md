# Vendored Windows polyfills

These directories contain unmodified third-party sources used only when
building under MSVC (`_WIN32`). On POSIX builds, nothing under `src/win/`
is compiled. Each subdirectory retains its upstream `LICENSE` file.

## wingetopt

- Upstream: <https://github.com/alex85k/wingetopt>
- Pinned at: tag `v1.00` (commit `effe6e2`, 2021-10-30)
- License: ISC-style (MIT-compatible) — see `wingetopt/LICENSE`
- Provides: `getopt()` for `cl.exe`, which has no `<unistd.h>`.

## dirent

- Upstream: <https://github.com/tronkko/dirent>
- Pinned at: tag `1.26` (commit `31db647`, 2025-07-15)
- License: MIT — see `dirent/LICENSE`
- Provides: `<dirent.h>` polyfill over `FindFirstFile`/`FindNextFile` for
  the recursive-add path in `pakfile.c`.

## Updating

Bump by replacing the file contents and updating the tag/commit/date
above. Do not edit the vendored files in place — patches against the
upstreams are out of scope here.
