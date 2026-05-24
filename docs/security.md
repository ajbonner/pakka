# Security

Pakka treats pak entry names as attacker-controlled data and runs every
extract, add, and verify through the same fail-fast validator. This
document is the byte-level reference for what the validator rejects and
why; the README has the short summary.

The validator (`pakka_unsafe_entry_name`) is format-independent — it
inspects the entry-name byte content, not the format-specific field
width — so a name that's legal in the on-disk directory can still be
rejected at extract or add time. The same rules apply to the entry-name
side of `pakka_add_file` / `--as`, so a pak built with pakka can be
re-extracted with pakka without surprises.

## Rejected entry names

* **Empty names.**
* **Absolute paths.** Names beginning with `/` or `\`.
* **Drive-letter prefixes.** `C:...`, `D:...`, ...
* **UNC paths.** `\\server\share\...`.
* **Parent-directory components.** Any path component that is exactly
  `..` (slash- or backslash-separated). Substring matches like
  `foo..bar` or `..png` remain legal — only an exact `..` component is
  rejected.
* **Colon in a component.** Windows alternate-data-stream hazard
  (`file:stream`).
* **Trailing dot or space in a component.** Windows silently strips
  these, so `foo.` and `foo` would collide.
* **Control bytes.** `0x00`–`0x1F` or `0x7F` anywhere in the name.
* **Windows reserved device names.** `CON`, `PRN`, `AUX`, `NUL`,
  `COM0`–`COM9`, `LPT0`–`LPT9`, including with extensions (`NUL.txt`)
  and in subdirectories (`foo/CON`). Recent Windows versions accept
  `COM0` and `LPT0` but pakka rejects them for safety. `COM10` and
  beyond are allowed — only the single-digit forms are reserved.

Validation runs before any `mkdir`, `fread`, or `fopen`, so a malicious
pak fails fast with no partial writes. Both POSIX and Windows traversal
forms are checked regardless of host OS, since pak archives are
portable.

## Extraction-time protections

Three checks run beyond the entry-name validator when bytes would touch
disk:

### Normalized-collision preflight

Before any byte is written, pakka sorts the selected entries'
normalized names — case-folded, with `\` mapped to `/`, and trailing
dot or space stripped — and rejects the whole extraction if two entries
would materialize to the same path on Windows or HFS+.

Without this check, two entries that differ only in case or in a
trailing dot would silently overwrite each other on case-insensitive
filesystems.

### Symlink / reparse-point rejection during write

Every directory descent into the `-C` destination tree, and every leaf
open, refuses to cross a symlink or reparse point:

* **Modern POSIX** — `openat` with `O_NOFOLLOW` on each path component.
* **Legacy POSIX** (pre-`openat`, BSDs below the floors documented in
  [`platforms.md`](platforms.md)) — an `fchdir`-based emulation that
  re-`lstat`s each component.
* **Windows** — a `GetFileAttributesA`-based reparse check.

A planted symlink inside the destination directory cannot redirect a
write outside it.

### Symlink rejection on recursive add

When `pakka -a` recurses into a directory tree, any symlink or reparse
point found inside is reported and skipped rather than silently
followed. This applies symmetrically to the write-time protection: the
archive can never gain entries materialized from a redirected path.

## Decompression bombs (PK3 / PK4 / Daikatana)

For archives with compressed entries,
`pakka_set_max_decompressed_size(archive, max_bytes, err)` caps the
bytes any single `pakka_open_entry` or `pakka_read_entry_alloc` will
inflate. Default is 64 MiB; pass 0 to disable. This refuses
zip-bomb-style high-ratio entries before they hit RAM.

## Auditing without extracting

`pakka --verify` runs the entry-name validator and the
normalized-collision preflight against every entry, streams every
payload to confirm the directory's `offset` / `length` point at
readable bytes, and exits non-zero on any error-level finding. None of
those checks touch the destination filesystem, so an archive can be
audited safely before extraction.

Pass `PAKKA_VERIFY_DEEP` to `pakka_verify` (library) for per-entry
CRC32 and decompression checks on top of the structural walk — CRC for
PK3 / PK4; byte-count check for Daikatana, whose custom codec has no
CRC.
