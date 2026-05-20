# Vendored DEFLATE encoder (sdefl)

Micha Mettke's single-header DEFLATE encoder, vendored from
[`vurtun/lib`](https://github.com/vurtun/lib/blob/master/sdefl.h).

## Pinned at

Commit `d66260b1e7eb` (2025-03-15). Upstream is a single `sdefl.h`; the
declarations and implementation share the same file, with the impl
gated behind `SDEFL_IMPLEMENTATION`.

## License

Dual MIT / Public Domain (Unlicense), at the consumer's option — see
the header comment in `sdefl.h`. Compatible with pakka's MIT.

## Why this codec

- ~799 LOC single header (encoder only, no INFLATE — that's sinfl).
- No `malloc` — `struct pakka_sdefl` is the entire worktable (~80 KB
  hash + prev arrays), allocated by the caller.
- No SIMD intrinsics; `__builtin_clz` / `_BitScanReverse` are guarded
  with a portable fallback for hosts that lack them.
- Endianness-correct on big-endian s390x: every multi-byte load goes
  through `memcpy(&n, p, sizeof n)`, no `*(uint32_t*)p` casts.
- Produces raw RFC 1951 DEFLATE via `pakka_sdeflate()` (and zlib-
  wrapped DEFLATE via `pakka_zsdeflate()`, unused by pakka — ZIP LFH
  payload is raw DEFLATE).
- Compiles clean under MSVC /W4 /WX, gcc 3.0+ `-std=c99 -pedantic`,
  and on every CI target in `.github/workflows/test.yml`.

Compression ratio at the default level (5) sits within ~1 % of zlib
`-6` on representative game-asset corpora — adequate for PK3/PK4
where the goal is interoperability with `unzip`/Quake III tooling
rather than peak ratio.

## Local modifications

Two passes applied at vendor time:

1. **Identifier rename.** Every `sdefl` / `SDEFL` identifier was
   prefixed with `pakka_` / `PAKKA_`. The `pakka_` prefix is required
   by `make symbol-audit` (every defined global in `libpakka.a` must
   begin with `pakka_`). Applied via the rename script in the
   migration commit (see git history); summary of public surface:

   - `sdeflate` → `pakka_sdeflate`
   - `zsdeflate` → `pakka_zsdeflate` (vendored but unused)
   - `sdefl_bound` → `pakka_sdefl_bound`
   - `struct sdefl` → `struct pakka_sdefl` (plus all internal
     `struct sdefl_*` member types)
   - `SDEFL_*` macros → `PAKKA_SDEFL_*` (incl. `PAKKA_SDEFL_IMPLEMENTATION`,
     `PAKKA_SDEFL_LVL_DEF`, etc.)
   - Internal `sdefl_*` statics renamed for consistency, though they
     don't appear in `nm -g` and would pass symbol-audit unrenamed.

2. **Compilation unit split.** Added a small `sdefl.c` shim that
   defines `PAKKA_SDEFL_IMPLEMENTATION` before including `sdefl.h`.
   The upstream single-header pattern needs exactly one TU to compile
   the impl; the rest of pakka can include `sdefl.h` decl-only via
   the (unused, for now) decl-only path. The header itself was not
   structurally changed. The shim also `#include <intrin.h>` under
   `_MSC_VER` so MSVC's `_BitScanReverse` intrinsic has a real
   declaration — without it, `/W4 /WX` rejects the implicit-decl in
   `pakka_sdefl_ilog2`.

## Updating

1. Download `sdefl.h` from a future `vurtun/lib` commit.
2. Re-apply the rename pass — the migration commit's rename script
   is idempotent on a fresh upstream pull.
3. Bump the SHA pin above.
4. Verify `make symbol-audit` passes and `make test` is green.
5. Run `make slow-test` (Q3 demo PK3 round-trip via the zlib backend
   and the vendored backend) to catch any encoder regression that
   would produce streams Q3 / id-Tech-4 tooling can't decode.

## Files in this directory

- `sdefl.h` — vendored, renamed.
- `sdefl.c` — 2-line shim that compiles the impl.
- `VENDOR.md` — this file.
