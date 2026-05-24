---
title: Pakka Documentation
---

# Pakka documentation

Pakka is a C library (`libpakka`) for reading, writing, and editing
the archive formats used by id Software, Ritual, Ion Storm, and Valve
game engines — Quake 1 / 2 and Half-Life / GoldSrc PAK, SiN SPAK,
Daikatana PAK, Doom IWAD / PWAD, Quake 3 PK3, and Doom 3 PK4. The
`pakka(1)` command-line tool is a thin wrapper around the same API.

The [project README](https://github.com/ajbonner/pakka/blob/master/README.md)
is the quickstart and launching-off point. These pages are the
supporting reference.

## Reference

- [Installation](install.md) — build options, install layout,
  Windows MSVC build, DEFLATE backend selection.
- [Format support](formats.md) — compression policies, Daikatana
  codec, ZIP feature subset, links to per-format on-disk references.
- [Using libpakka from C](libpakka.md) — API inventory grouped by
  responsibility, structured-error model, threading and ownership
  rules.
- [Security](security.md) — full entry-name rejection rules,
  normalized-collision preflight, symlink / reparse handling,
  decompression-bomb cap.
- [Supported platforms](platforms.md) — full CI matrix, legacy
  floor, additional CI jobs (lint, fuzz, sanitizers, coverage,
  zlib-backend, win32 lint).
- [Testing](testing.md) — Windows CTest path, realpak opt-in
  suites, `make lint`.

## Source-level format references

For on-disk format internals — directory geometry, codec opcode
tables, byte ordering — see the
[`dev/docs/`](https://github.com/ajbonner/pakka/tree/master/dev/docs)
references in the repo. Those are aimed at contributors working on
pakka itself.

## Releases

See the
[GitHub releases page](https://github.com/ajbonner/pakka/releases)
for source tarballs and pre-built binaries (Windows x64 / x86, macOS
Intel / Apple Silicon), each with SHA-256 sidecars.

## License

MIT — see
[`LICENSE`](https://github.com/ajbonner/pakka/blob/master/LICENSE) in
the repository.
