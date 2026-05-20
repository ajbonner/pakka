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
- Endianness-correct on big-endian s390x: the upstream `memcpy(&n,
  p, 8)` would have returned host-order bytes (load-bearing bug on
  BE — DEFLATE is LSB-first); pakka's local patch in
  `pakka_sinfl_read64` replaces that with an explicit LE byte-shift
  load. No unaligned u64 casts that would trip `-Wpedantic`.
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

1. **No bytes-consumed surface.** sinfl returns only the output
   count; the wrapper reports `*in_consumed == src_len` on success.
   Trailing-bytes detection inside an LFH's declared csize is
   therefore softer than puff's (zlib backend has exact `z.total_in`).
   `pakka_pk3_deep_verify_entry`'s CRC32 cross-check is the end-to-
   end integrity backstop.
2. **Soft-fail on some malformed inputs.** sinfl returns a partial-
   output success count for invalid block types, stored-block
   LEN/NLEN mismatch, and invalid Huffman code lengths. The caller's
   `written != entry->length` check catches every case where the
   malformed block appears before the declared usize is reached;
   CRC32 in deep-verify catches the rest.

**Local mod — bounded refill (fixes an upstream OOB).** Upstream
`pakka_sinfl_refill` reads 8 bytes ahead via `memcpy` without
checking input bounds. Adversarial or tampered DEFLATE streams can
drive bitptr arbitrarily far past the actual input end before the
state machine notices, producing a heap-buffer-overflow read
(reproducible under ASan with the existing PK3 test fixtures). The
patch adds an `s->bitend` field (one-past-last input byte), sets it
in `pakka_sinfl_decompress`, and has `pakka_sinfl_refill` clamp both
the read length and the bitptr advance against `bitend`. The tail
read zero-fills the missing bytes — DEFLATE end-of-block /
invalid-symbol detection already handles the resulting bit pattern
via its existing checks. See `src/vendor/sinfl/sinfl.h` for the
patched code; the change is small enough to re-apply mechanically
on an upstream rebase.

**Local mod — explicit LE byte-shift in pakka_sinfl_read64.**
Upstream `memcpy(&n, p, 8)` reads bytes in HOST byte order. The
DEFLATE bitstream is LSB-first and requires byte 0 of input to land
in the low byte of `bitbuf` so bit 0 of byte 0 == bit 0 of bitbuf —
correct on little-endian, inverted on big-endian. The
`linux-glibc-s390x-be` CI job decodes garbage without this patch.
Replaced the memcpy with an explicit 8-byte LE shift load. Cost is
a few cycles per refill (negligible against Huffman decode).

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
