# Vendored &lt;dirent.h&gt; for MSVC (tronkko/dirent)

Header-only `<dirent.h>` polyfill over `FindFirstFile` / `FindNextFile`.
Used only when `_WIN32` is defined; CMake adds the include directory
on Windows, POSIX builds skip it.

## Pinned at

- Upstream: <https://github.com/tronkko/dirent>
- Tag `1.26` (commit `31db647`, 2025-07-15)

## License

MIT — see `LICENSE` in this directory.

## Used by

`cli_add_folder_r` in `src/cli.c` for the recursive directory walk
when adding folders to an archive.

## Updating

Bump by replacing the file contents and updating the tag/commit/date
above. Do not edit the vendored files in place — patches against the
upstream are out of scope here.
