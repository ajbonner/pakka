# Vendored getopt for MSVC (wingetopt)

`getopt()` polyfill for `cl.exe`, which has no `<unistd.h>`. Compiled
only when `_WIN32` is defined; CMake adds `getopt.c` to the CLI source
list, POSIX builds skip it.

## Pinned at

- Upstream: <https://github.com/alex85k/wingetopt>
- Tag `v1.00` (commit `effe6e2`, 2021-10-30)

## License

ISC-style (MIT-compatible) — see `LICENSE` in this directory.

## Used by

`src/cli.c` for command-line argument parsing under MSVC.

## Updating

Bump by replacing the file contents and updating the tag/commit/date
above. Do not edit the vendored files in place — patches against the
upstream are out of scope here.
