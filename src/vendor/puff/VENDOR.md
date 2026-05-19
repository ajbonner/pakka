# Vendored INFLATE decoder (puff)

Mark Adler's reference INFLATE implementation, vendored from
[`madler/zlib`](https://github.com/madler/zlib/tree/develop/contrib/puff).

## Pinned at

Commit `9e35567064ba` (zlib version 1.3.2 bump, 2026-02-17). The
puff source itself is version 2.3 (21 Jan 2013) and hasn't changed
in over a decade — the zlib commit is what determined the SHA pin.

## License

zlib license — see header comment in `puff.h`. BSD-2-style: free use,
modification, redistribution, no warranty. Compatible with pakka's
MIT.

## Why this codec

- Public domain author's BSD-equivalent license.
- ~840 LOC, single file plus a 35-line header.
- No compiler intrinsics, no `__builtin_*`, no `__attribute__`.
- No malloc — every working buffer is on the stack (<2 KB).
- Designed for portability over speed (RFC 1951 reference).
- Endianness-correct on big-endian hosts by construction.
- Compiles clean under `c99 --pedantic -Wall`.

The speed cost (puff is ~10× slower than zlib's tuned `inflate()`) is
irrelevant for pakka's "extract on user invocation" workflow.

## Local modification

The only public symbol `puff()` was renamed to `pakka_inflate()` so it
passes pakka's `make symbol-audit` gate (every exported global must
begin with `pakka_`). The rename was applied via:

    sed 's/\bpuff\b/pakka_inflate/g' upstream/puff.{c,h}
    # then restored the #include "puff.h" line which the rename
    # accidentally caught

Statics are unaffected (they keep the upstream names; `static` linkage
means they don't appear in `nm -g`).

## Updating

1. Download `contrib/puff/puff.c` and `contrib/puff/puff.h` from a
   future zlib release.
2. Re-apply the `puff` → `pakka_inflate` rename.
3. Bump the SHA pin and zlib version above.
4. Verify `make symbol-audit` still passes and the existing PK3 and
   PK4 bats suites are green.

## Files in this directory

- `puff.c` — vendored, renamed.
- `puff.h` — vendored, renamed.
- `VENDOR.md` — this file.
