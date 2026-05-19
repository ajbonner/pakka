# PK3 Support — Implementation Design

Read + STORED-write support for Quake 3 / Doom 3 PK3 archives in
pakka. DEFLATE encode is deferred. Vendored INFLATE codec is Mark
Adler's `puff.c` (zlib contrib/, public domain).

## What ships

Public API surface stays at 20 functions plus one new setter. The
`PAKKA_FORMAT_PK3` enumerator flips from "rejected at open/create" to
"supported." All 20 existing functions transparently handle PK3
archives once `archive->format == PAKKA_FORMAT_PK3`.

New public symbols:

```c
pakka_status_t pakka_set_max_decompressed_size(
    pakka_archive_t *archive, uint64_t max_bytes);

/* For pakka_verify's flags parameter. */
#define PAKKA_VERIFY_DEEP 0x1u
```

`max_decompressed_size` defaults to **64 MiB**. Enforced at
`pakka_open_entry` / `pakka_read_entry_alloc` time (not at CDR parse
— the setter takes an already-open archive, and per-entry decompression
is where the actual allocation happens). Set to 0 to disable.

The 64 MiB default is bounded by `SIZE_MAX` and `ULONG_MAX` (puff
returns `unsigned long` lengths). 32-bit hosts get the same default.
Quake 3 demo `pak0.pk3` largest single entry is ~5 MB
(`models/players/sarge/h_head.md3`-class assets); 64 MiB has ample
headroom.

`PAKKA_VERIFY_DEEP` is the flag bit for deep verify (decompress + CRC
check every entry). Default `pakka_verify(... flags=0 ...)` is
structural-only.

CLI: extension-based format detection on `-c` (`.pk3` → PK3, anything
else → PAK). Optional `--format pk3` override for ambiguous filenames.

## What does NOT ship

| Feature | Why deferred |
|---|---|
| DEFLATE encode | Real portable encoders pull in `__builtin_clz` / `_BitScanReverse` fallbacks and ~1500 LOC. Real-world demand is small (Quake engines accept STORED). |
| ZIP64 | Lifts 4 GiB cap. Returns `PAKKA_ERR_UNSUPPORTED` with a clear message; revisit when someone actually has a >4 GiB PK3. |
| Encrypted ZIP | Refused with `PAKKA_ERR_UNSUPPORTED`. Quake content has no encrypted entries in the wild. |
| Multi-disk ZIP | Refused. Same rationale. |
| Data descriptors (general purpose bit 3) | Refused — they require streaming-without-seek, which doesn't fit pakka's seekable-FILE model. CDR is authoritative; LFH must have real sizes. |

## Codec: `puff.c`

Vendored from [`madler/zlib/contrib/puff`](https://github.com/madler/zlib/tree/develop/contrib/puff).
Public domain (zlib's license excludes contrib/puff, which carries its
own zlib-license header — still BSD-equivalent). ~700 LOC single file.
By zlib's author. 20+ years stable. No intrinsics. Reads bitstream
byte-at-a-time, so endianness-correct on BE hosts by construction.

Lands at `src/vendor/puff/puff.c` and `src/vendor/puff/puff.h`. A
`src/vendor/puff/VENDOR.md` records the upstream commit SHA — same
per-subdir pattern used for the other `src/vendor/*/` entries
(wingetopt, dirent).

puff exposes one entry point: `int puff(unsigned char *dest, unsigned
long *destlen, const unsigned char *source, unsigned long *sourcelen)`.
Returns 0 on success, negative on format errors, positive on truncation.

### Symbol-audit accommodation

`puff` and its statics aren't `pakka_`-prefixed. Two options:

1. Rename `int puff(...)` → `int pakka_inflate(...)` in the vendored
   copy, document the rename in `VENDOR.md`, statics keep their names
   (they're `static`, so `nm -g` won't see them).
2. Wrap puff in `src/pk3file.c` and mark puff's externs `static`
   (the file is tiny, just include the .c directly).

Going with option 1. Rename happens once at vendor time, recorded in
VENDOR.md, no further drift. Option 2 forces an `#include "puff.c"`
which clang-tidy rightly flags.

## Container layer: `src/pk3file.c`

New TU, peer of `src/pakfile.c`. Holds internal helpers only — no
public `pakka_*` entry points live here, to keep the symbol-audit
gate trivially correct. Helpers are named `pakka_pk3_*` so the
audit's name-prefix check still passes.

The existing public `pakka_open` / `pakka_create` / `pakka_close` /
... functions in `src/pakfile.c` get a format-dispatch prologue:

```c
pakka_status_t pakka_open(...) {
    /* sniff first 4 bytes */
    if (signature is PK3) return pakka_pk3_open_impl(...);
    /* fall through to PAK loader */
}
```

This keeps the public surface in one TU (single source of truth,
no duplicate-symbol risk) and the PK3-specific guts in `pk3file.c`.

### EOCD scan + validation

ZIP records the central directory location in the End of Central
Directory record at the *end* of the file. EOCD signature is `PK\005\006`,
record is 22 bytes plus up to 64 KiB of trailing comment. Scan
backwards from EOF for the signature; cap the scan at 65557 bytes (64
KiB + 22 + a slack byte) to refuse pathological inputs.

Once a candidate EOCD is found, run the full validation list before
trusting it:

1. EOCD comment length matches the bytes after EOCD up to EOF (refuse
   trailing garbage).
2. Disk number == 0 and disk-with-CDR == 0 (refuse multi-disk).
3. CDR entry count on this disk == total CDR entry count (refuse
   inconsistent counts).
4. CDR offset + CDR size ≤ file size (refuse CDR past EOF).
5. No ZIP64 sentinels: `entries == 0xffff`, `cdr_size == 0xffffffff`,
   `cdr_offset == 0xffffffff` all refused with `PAKKA_ERR_UNSUPPORTED`.
6. CDR entry count ≤ `PAKFILE_MAX_ENTRIES` (1 M, same cap as PAK).

If multiple `PK\005\006` matches exist (theoretically possible since
the signature could appear inside the comment), the *last* valid one
wins per the ZIP spec — implemented by scanning EOF→start, taking the
first match that passes the validation list above.

### Central directory parse

Each CDR entry is a variable-size record. Critical fields:

| Offset | Size | Field | Pakka use |
|---|---|---|---|
| 0 | 4 | Signature `PK\001\002` | Validate |
| 4 | 2 | Version made by | Ignored |
| 6 | 2 | Version needed | Refuse if > 20 (no ZIP64, no AES) |
| 8 | 2 | General purpose bit flag | Refuse encrypted (bit 0), data desc (bit 3) |
| 10 | 2 | Compression method | Accept 0 (STORED), 8 (DEFLATE); refuse else |
| 12 | 4 | DOS mod time/date | Ignored |
| 16 | 4 | CRC32 | Stored, used in verify |
| 20 | 4 | Compressed size | Stored |
| 24 | 4 | Uncompressed size | Stored |
| 28 | 2 | File name length | ≤ PAKKA_ENTRY_NAME_SIZE - 1, else `PAKKA_ERR_LIMIT` |
| 30 | 2 | Extra field length | Skipped |
| 32 | 2 | Comment length | Skipped |
| 34 | 2 | Disk number | Refuse if non-zero |
| 36 | 2 | Internal attrs | Ignored |
| 38 | 4 | External attrs | Top 16 bits = Unix mode; refuse if `S_IFLNK` set |
| 42 | 4 | LFH offset | Stored |
| 46 | N | File name | Validated via `pakka_unsafe_entry_name` |

### Per-entry storage

Single storage convention for ALL formats. The hybrid `name_ptr +
filename[]` from an earlier draft was too clever; one branch per
accessor across every lookup / verify / report path is real bug
surface for marginal memory savings.

```c
struct pakka_entry {
    char name[PAKKA_ENTRY_NAME_SIZE];   /* 256-byte inline; PAK fits in 56 */
    uint32_t offset;                    /* unchanged from today */
    uint32_t length;                    /* unchanged from today */
    struct pakka_entry *next;

    /* PK3-only. Zero for PAK. Sized u32 — pakka v1 does not support
       ZIP64, so individual entries cap at 4 GiB exactly like PAK
       does. */
    uint32_t pk3_compressed_size;
    uint32_t pk3_payload_offset;        /* cached LFH + 30 + name + extra */
    uint32_t pk3_lfh_offset;            /* for verify cross-check */
    uint32_t pk3_crc32;
    uint16_t pk3_method;                /* 0 = STORED, 8 = DEFLATE */
};
```

Net per-entry growth: 256 + 18 = 274 bytes vs. today's 65. On id's
`pak0.pak` (339 entries) that's ~90 KB total entry-list footprint.
Acceptable.

`pakka_entry_size` / `pakka_entry_offset` accessors widen their
return type to `uint64_t` only on the way out — internal storage
stays `uint32_t`. The existing public accessor signatures already
return `uint64_t`, so no header change.

### Streaming reader

`struct pakka_reader` grows fields for the deflate path:

```c
struct pakka_reader {
    struct pakka_archive *archive;
    uint64_t next_offset;
    uint64_t remaining;          /* uncompressed bytes left to deliver */

    /* PK3-only. NULL for PAK and for STORED PK3 entries. */
    unsigned char *inflated_buf;     /* whole decompressed payload */
    size_t inflated_len;             /* == entry->length */
    size_t inflated_cursor;          /* read position into inflated_buf */
};
```

For STORED entries (and PAK), `pakka_reader_read` is structurally
identical: seek + fread, no decompression. For DEFLATE entries:

1. On first `pakka_reader_read` after `pakka_open_entry`, allocate
   `inflated_buf` of size `entry->length` (uncompressed_size from
   CDR). Refuse if `length > archive->max_decompressed_size`.
2. Read entire compressed payload from disk into a temporary buffer
   of size `pk3_compressed_size`. Refuse if `pk3_compressed_size >
   max_decompressed_size` (the cap covers both directions to bound
   peak RSS).
3. Call `pakka_inflate` (renamed puff). On success, free the
   compressed buffer; keep `inflated_buf` for read-out.
4. `pakka_reader_read` memcpys from `inflated_buf + inflated_cursor`.

This is whole-buffer inflate, not streaming. puff doesn't offer a
streaming API. The 64 MiB default cap means peak RSS per reader is
bounded by 128 MiB (compressed + decompressed during step 3).
`pakka_reader_close` frees `inflated_buf`.

CRC32 is NOT verified on normal reads. Verifying on `pakka_reader_read`
would force whole-entry buffering for STORED entries too (we'd have to
defer the read complete signal until CRC matches). CRC verification
is the job of `pakka_verify --deep` (or the C-API equivalent
`pakka_verify(... PAKKA_VERIFY_DEEP ...)`).

### Write path (STORED)

`pakka_create(path, PAKKA_FORMAT_PK3, ...)` writes a minimal valid
empty PK3: an EOCD with zero central directory entries at offset 0.
Same pattern as the empty-PAK case in `pakfile.c`.

The archive tracks `pk3_cdr_start` — the file offset where the
central directory begins (== where the EOCD currently sits). When
the first `pakka_add_*` runs:

1. `fseek` to `pk3_cdr_start`. This effectively truncates the file
   at the old CDR position, so the LFH about to be written replaces
   the old CDR + EOCD rather than appending after them.
2. Validate entry name (`pakka_unsafe_entry_name`).
3. Compute CRC32 over the source bytes. Static 256-entry table,
   built once via `static` initializer or first-use lazy build.
4. Write Local File Header. Method = 0 (STORED), compressed_size ==
   uncompressed_size == source byte count.
5. Stream source bytes after the LFH.
6. Append entry to in-memory list with `pk3_method = 0`,
   `pk3_lfh_offset = LFH_pos`, `pk3_payload_offset = LFH_pos + 30 +
   name_len`, `pk3_crc32`, `pk3_compressed_size = length`.
7. Update `pk3_cdr_start` to current file position.
8. Mark dirty. CDR + EOCD will be written by `pakka_commit`.

`pakka_commit` for PK3 writes the central directory at `pk3_cdr_start`
followed by the EOCD record, then `ftruncate` to drop any trailing
bytes (defensive — should be exact already).

`pakka_delete` triggers the same temp-rebuild pattern as PAK. The
rebuild **copies LFH + compressed payload bytes verbatim** — does
NOT inflate and re-store. Preserves method, CRC, sizes exactly. This
matters: re-storing a DEFLATE entry as STORED would silently inflate
the on-disk pak.

### Format detection in `pakka_open`

The existing PK3-signature check at `src/pakfile.c:518` currently
returns `PAKKA_ERR_UNSUPPORTED`. Replace with a call into
`pk3_open_impl` that does the EOCD scan and central directory parse.
For `PK\005\006` (empty ZIP with EOCD only) the scan succeeds with
zero entries.

Detection logic:

```
read first 4 bytes:
  "PACK"          → PAK loader
  "PK\003\004"    → PK3 loader (LFH at start, typical case)
  "PK\005\006"    → PK3 loader (empty PK3)
  "PK\007\008"    → reject as multi-disk (refused above)
  anything else   → PAKKA_ERR_FORMAT
```

A few PK3s in the wild start with bytes other than `PK\003\004` —
they have data before the first LFH (e.g. self-extracting wrapper).
Pakka treats only `PK\003\004` and `PK\005\006` as valid PK3 starts;
the EOCD scan finds the CDR regardless. Self-extracting PK3s with
non-`PK*` leading bytes are out of scope.

### LFH validation (at open, not at read)

For every CDR entry, at open time:

1. Seek to `pk3_lfh_offset`. Read 30-byte LFH.
2. Validate signature `PK\003\004`.
3. Validate LFH method == CDR method.
4. Validate LFH flags don't have encryption / data-descriptor bits.
5. Validate LFH compressed_size == CDR compressed_size,
   uncompressed_size == CDR uncompressed_size, CRC32 == CDR CRC32.
   (If LFH carries zeros because bit 3 was set, refuse — bit 3 is
   already in the refused-flags list above.)
6. Validate LFH filename matches CDR filename byte-for-byte.
7. Cache `pk3_payload_offset = lfh_offset + 30 + lfh_name_len +
   lfh_extra_len`. Validate `pk3_payload_offset + compressed_size <=
   file_size`.

Any failure → entire archive rejected with `PAKKA_ERR_FORMAT`.

### `pakka_verify` for PK3

Default (structural):

1. Entry name safety → reuse `pakka_unsafe_entry_name`.
2. Method is one of STORED / DEFLATE (already enforced at open;
   verify confirms the in-memory entry list is consistent).
3. Normalized-collision preflight — reuse
   `pakka_normalize_entry_name`, sort by normalized name, flag
   duplicates.
4. Exact-duplicate names — flag (already rejected at open in pakka's
   PAK code via `load_directory`; mirror for PK3).

With `PAKKA_VERIFY_DEEP`:

5. Decompress every entry through `pakka_inflate`, confirm produced
   bytes == uncompressed_size.
6. Compute CRC32 of decompressed bytes, confirm match.

## Safety boundary

Reuses existing infrastructure:

| Check | Provider | PK3 reuse |
|---|---|---|
| Path traversal, drive prefix, control bytes, Windows reserved names | `pakka_unsafe_entry_name` | unchanged |
| Normalized collision (case, slash, dot/space) | `pakka_normalize_entry_name` | unchanged |
| Symlink-safe extract on POSIX/Windows | `openat`/`O_NOFOLLOW` / reparse check in `src/cli.c` | unchanged — happens at extract time, format-agnostic |
| ZIP symlink entries (Unix mode bit) | new — refuse at CDR parse time | PK3-only |

New PK3-specific checks:

- ZIP64 sentinels (`0xffff` / `0xffffffff`) → refuse with
  `PAKKA_ERR_UNSUPPORTED`.
- Encrypted (general bit 0) → refuse.
- Data descriptors (general bit 3) → refuse.
- Multi-disk (disk number != 0) → refuse.
- Compression method other than 0 or 8 → refuse.
- Version-needed > 20 → refuse.
- Entry's external-attrs Unix file-type mask `0170000` == symlink
  bits `0120000` → refuse. Use a raw mask defined locally (`#define
  PK3_UNIX_IFMT 0170000u`, `#define PK3_UNIX_IFLNK 0120000u`), not
  `<sys/stat.h>` macros — MSVC's `<sys/stat.h>` doesn't define
  `S_IFLNK`.
- Directory entries (CDR name ends in `/`, uncompressed_size == 0):
  silently skipped — don't appear in `pakka_entry_count`, don't
  count toward the 1 M cap. ZIP tools commonly emit them; refusing
  them would reject real-world PK3s.
- Bit 11 (UTF-8 filename): accepted but not normalized. Pakka stores
  the name's raw bytes; safety validator runs on those bytes. CP437
  names with high-bit characters are also accepted (Quake content
  sometimes has these).
- `max_decompressed_size` cap enforced at `pakka_open_entry` and
  `pakka_read_entry_alloc` time. Both compressed and uncompressed
  sizes are checked against the cap — covers peak RSS during inflate.

## Build & CI changes

### Makefile

```diff
-SOURCES=$(wildcard $(SRC_DIR)/*.c)
+SOURCES=$(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/vendor/puff/*.c)
```

The auto-discovered objects flow through `LIB_SOURCES` (filters out
`cli.c`), so puff lands in `libpakka.a` automatically.

New fixture for Q3 demo PK3:

```makefile
Q3_URL=https://archive.org/download/quake-3-demo/quake3-1.11-6_demo.x86.run
Q3_SHA256=...
Q3_RUN=$(TEST_DIR)/q3demo.run
Q3_PAK0_PK3=$(TEST_DIR)/q3demo/demoq3/pak0.pk3
```

The Q3 demo is freely redistributable (id released it in 2000). The
installer is a `.run` shell-archive; extraction uses a 12-line C
helper at `tests/extract_run.c` (NOT `tail +N`, which is non-portable —
BSD `tail` doesn't take `+N`). The helper scans for the `tar.gz`
magic and dd's bytes from there. Test setup mimics the existing
`$(QUAKE_TARBALL)` / `verify-tarball` / `$(PAK0)` pipeline.

Gated behind a `slow-test` target:

```makefile
slow-test: $(TARGET) $(Q3_PAK0_PK3) symbol-audit
	bats tests/pk3-q3demo.bats
```

`make test` stays fast (synthetic fixtures only). `make slow-test`
hits the real Q3 PK3. archive.org is intermittently 503; if the
fetch fails, `make test` keeps working — only `make slow-test` is
affected.

### CMakeLists.txt

Add `src/vendor/puff/puff.c` to the static-library source list.
Mirror the Makefile change.

### CI matrix

No new jobs. Every existing job runs the synthetic PK3 fixture suite
through `make test`. The big-endian s390x and NetBSD/sparc jobs
validate puff's endianness handling. The `-m32` lanes validate that
the 64-bit `uint64_t` offset/length fields don't break 32-bit builds
(they were already 64-bit in the public API, but PK3 is the first
format that actually uses values > 4 GiB potential).

Q3 demo job: deferred. Could add a single `q3-demo-suite` job that
runs `make slow-test` on Ubuntu glibc x86_64 only. Not blocking v1.

## Files touched

| File | Change |
|---|---|
| `include/pakka.h` | Add `pakka_set_max_decompressed_size` prototype. `PAKKA_FORMAT_PK3` already exists. |
| `src/common.h` | Extend `pakka_entry` with PK3 fields. Extend `pakka_archive` with `format` field + EOCD-cached offsets. Extend `pakka_reader` with inflate context. |
| `src/pakfile.c` | Format dispatch in `pakka_open` / `pakka_create` / etc. PAK code stays as-is. |
| `src/pk3file.c` | New. ZIP container code, calls into vendored puff for DEFLATE. |
| `src/vendor/puff/puff.c` | New. Vendored from madler/zlib contrib/. |
| `src/vendor/puff/puff.h` | New. |
| `src/vendor/puff/VENDOR.md` | New. Pinned commit, license note, rename log. |
| `src/cli.c` | Extension-based format detection on `-c`; `--format pk3` flag. |
| `Makefile` | Vendor glob, Q3 demo fixture, `slow-test` target. |
| `CMakeLists.txt` | Add puff.c to static lib sources. |
| `tests/pk3.bats` | New. Synthetic PK3 fixture tests. |
| `tests/pk3-q3demo.bats` | New. Q3 demo fixture test (slow). |
| `tests/c_api_test.c` | Add PK3 round-trip scenarios. |
| `README.md` | Update Usage / Known Limitations / Supported Formats. |
| `CLAUDE.md` | Architecture section: PK3 module, vendor layout. |
| `dev/comparison.md` | Update PK3 gap row and Things-only-pakka-has. |

## Risk register

1. **puff license clarity.** zlib's main license excludes contrib/.
   Need to verify puff.c's header is BSD/zlib-equivalent (it is — Mark
   Adler explicitly placed it under zlib license terms with the
   "may be reproduced freely" preamble). Documented in VENDOR.md.
2. **Q3 demo URL stability.** Archive.org has been hosting it for
   years but URLs aren't immortal. SHA-pinned + fallback mirror noted
   in Makefile comments.
3. **PK3 entries with backslash separators.** Some Windows-built PK3s
   use `\` instead of `/`. Pakka's `pakka_unsafe_entry_name` already
   handles both. `pakka_normalize_entry_name` already folds them.
4. **CRC32 implementation choice.** A static 256-entry table is the
   common approach. Initialized at first use. No dynamic CRC tables,
   no slice-by-N optimizations. ~40 LOC.

## Test plan

Synthetic PK3 fixtures (built by `tests/setup_pk3.sh`, runs at test
time):

| Fixture | Contents | Test purpose |
|---|---|---|
| `empty.pk3` | EOCD only, zero entries | open + list + entry_count == 0 |
| `single-stored.pk3` | One STORED entry | open + extract + read_entry_alloc |
| `single-deflate.pk3` | One DEFLATE entry (small payload) | INFLATE path |
| `mixed.pk3` | Multiple STORED + DEFLATE entries | iteration + per-entry method dispatch |
| `traversal.pk3` | Entry named `../etc/passwd` | extract refuses |
| `reserved.pk3` | Entry named `CON.txt` | extract refuses on every host |
| `symlink.pk3` | Entry with Unix mode `S_IFLNK` set | extract refuses |
| `bomb.pk3` | Entry declaring 10 GiB uncompressed | refuses (>4 GiB default cap) |
| `collide.pk3` | Two entries `foo` and `FOO/` | normalized-collision preflight refuses |

Q3 demo:

- `pak0.pk3` round-trip: extract all, verify count matches, verify
  CRC32 on a few representative entries.
- `pakka --verify pak0.pk3` succeeds with no ERROR-level findings.

C-API test:

- Round-trip create-via-add → close → re-open → list → extract → byte-
  compare against source.
- Streaming reader on a DEFLATE entry: read in small chunks, confirm
  uncompressed bytes match.
- `pakka_set_max_decompressed_size` rejects oversize entries.
- NULL tolerance on every new public symbol.

## Codex plan-review decisions

Codex's review (`/tmp/codex-pk3-plan-review.md`) flagged 10 design
concerns. Resolutions:

1. **Setter phase** — `pakka_set_max_decompressed_size` enforces at
   `pakka_open_entry` / `pakka_read_entry_alloc` only, not at CDR
   parse. Reflected in the safety table above.
2. **64 MiB default cap**, not 4 GiB. Applies to both compressed and
   uncompressed side.
3. **Public functions stay in `pakfile.c`**; `pk3file.c` exposes only
   `pakka_pk3_*` internal helpers. No duplicate public symbols.
4. **No uint64_t widening** of internal `pakka_entry` offset/length.
   Stay `uint32_t`; public accessors already return `uint64_t`.
5. **Single inline `name[256]`** for all entries, no hybrid pointer.
6. **EOCD validation list** (multi-disk, counts, sentinels, bounds).
7. **LFH validated at open**, not at read; cache `pk3_payload_offset`.
8. **CDR start tracked**, not "append after EOCD"; rewrite from
   `pk3_cdr_start` on every mutation.
9. **`PAKKA_VERIFY_DEEP` defined now**, not "reserved".
10. **Portability — `S_IFLNK` replaced with raw `PK3_UNIX_IFLNK` mask**,
    `tail +N` replaced with 12-line C helper, `puff.c` builds tested
    under `/W4 /WX` and `--pedantic` (if MSVC complains, vendored
    pragma suppression goes in a wrapper header `puff_compat.h`).

Codex's missed-items list also accepted:

- Exact-duplicate PK3 names refused on open (mirrors PAK).
- Directory entries (trailing `/`, size 0) silently skipped.
- CRC mismatch on normal reads: NOT checked. Deep verify only.
- UTF-8 / bit 11: accepted, no normalization; safety validator runs
  on raw bytes.
- CDR cap == `PAKFILE_MAX_ENTRIES` (1 M).
- Rebuild copies LFH + payload verbatim, never re-inflates.
- Public docs explicitly say PK3 write = STORED-only.
- New setter has the same NULL/error tests + symbol-audit +
  header-lint coverage as every other public function.
