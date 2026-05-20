# Vendored DEFLATE decoder (sinfl)

Micha Mettke's single-header DEFLATE decoder, vendored from
[`vurtun/lib`](https://github.com/vurtun/lib/blob/master/sinfl.h).
Replaces Mark Adler's `puff` (zlib contrib) as pakka's default
DEFLATE decoder.

## Pinned at

Commit `5a3f3aba052e` (2025-03-19). Upstream is a single `sinfl.h`
with declarations and implementation in one file, gated by
`SINFL_IMPLEMENTATION`.

## License

Dual MIT / Public Domain (Unlicense), at the consumer's option — see
the header comment in `sinfl.h`. Compatible with pakka's MIT and with
the `sdefl` sibling.

## Why this codec

Same portability properties as `sdefl` (its encoder sibling):

- ~621 LOC single header.
- No `malloc`; all state is stack-local in `pakka_sinflate()`.
- Endianness-safe via `memcpy(&n, p, 8)` reads — no unaligned u64
  casts that would trip `-Wpedantic` or break on big-endian s390x.
- Optional NEON / SSE2 fast paths are gated behind
  `PAKKA_SINFL_NO_SIMD`. **pakka forces `PAKKA_SINFL_NO_SIMD` on**
  in `sinfl.c` because: (a) the CI matrix runs big-endian s390x and
  legacy NetBSD/sparc that don't have the SIMD glue, (b) MSVC arm64
  doesn't expose `_mm_*` and would fail at link, (c) the portable
  byte-loop is fast enough — pakka's largest decode is ~5 MiB of Q3
  demo asset, under a second on every supported host.
- Same `__builtin_clz` / `_BitScanReverse` guarding with portable
  fallback as `sdefl`.

Raw RFC 1951 DEFLATE: `pakka_sinflate(out, cap, in, size)` returns
the decoded byte count or a negative error code.

Two caller-side caveats that the vendored backend wrapper documents
(see `src/deflate/deflate_vendored.c`):

1. **8-byte tail padding required.** `pakka_sinfl_refill` reads 8
   bytes at a time via `memcpy`. Pakka's call sites pass tight LFH-
   sized buffers, so `deflate_vendored.c` copies into a padded
   scratch (`src_len + 8` zero-padded) before invoking sinfl.
2. **No bytes-consumed surface.** sinfl returns only the output
   count; the wrapper reports `*in_consumed == src_len` on success.
   Trailing-bytes detection inside an LFH's declared csize is
   therefore softer than puff's (zlib backend has exact `z.total_in`).
   `pakka_pk3_deep_verify_entry`'s CRC32 cross-check is the end-to-
   end integrity backstop.
3. **Soft-fail on some malformed inputs.** sinfl returns a partial-
   output success count for invalid block types, stored-block
   LEN/NLEN mismatch, and invalid Huffman code lengths. The caller's
   `written != entry->length` check catches every case where the
   malformed block appears before the declared usize is reached;
   CRC32 in deep-verify catches the rest.

Why replace puff? Author symmetry with sdefl — same code style,
same dual MIT/PD license, same memcpy-based load pattern, same
portability bar. Net code reduction: ~840 LOC puff → ~621 LOC sinfl.
puff's RFC-reference pedigree was attractive, but the Q3 demo PK3
fixture + the existing PK3 round-trip bats suite give us our own
regression coverage independent of upstream auditing history. See
the migration commit for the rollback procedure if a real-world
PK3 ever fails to decode through sinfl.

## Local modifications

1. **Identifier rename.** Every `sinfl` / `SINFL` symbol was
   prefixed with `pakka_` / `PAKKA_` (same rule as `sdefl`).
   Summary:

   - `sinflate` → `pakka_sinflate`
   - `zsinflate` → `pakka_zsinflate` (vendored but unused — ZIP
     payloads are raw DEFLATE, not zlib-wrapped)
   - `struct sinfl` → `struct pakka_sinfl`
   - `struct sinfl_gen` → `struct pakka_sinfl_gen`
   - `SINFL_*` macros → `PAKKA_SINFL_*` (incl. the
     `PAKKA_SINFL_IMPLEMENTATION` / `PAKKA_SINFL_NO_SIMD` toggles
     that the shim consumes).

2. **Compilation unit split + SIMD off.** Added a 3-line `sinfl.c`
   that defines `PAKKA_SINFL_NO_SIMD` and `PAKKA_SINFL_IMPLEMENTATION`
   before including `sinfl.h`.

## Updating

Same procedure as `sdefl`'s VENDOR.md. Pay particular attention to
the SIMD guards — upstream periodically adds new fast paths under
new `#ifndef SINFL_*` macros; new gates need to be defined as
`PAKKA_SINFL_*` and turned off in `sinfl.c`.

## Files in this directory

- `sinfl.h` — vendored, renamed, SIMD code present but disabled by
  the shim's `PAKKA_SINFL_NO_SIMD`.
- `sinfl.c` — 3-line shim that compiles the impl with SIMD off.
- `VENDOR.md` — this file.
