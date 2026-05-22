# PK3 / PK4 format

A reference for the `.pk3` (Quake 3, 1999) and `.pk4` (Doom 3, 2004)
archive formats as implemented by pakka.

PK3 and PK4 are **byte-identical ZIP containers**. The only thing the
filename extension changes is the `pakka_format_t` label the caller
sees back (`PAKKA_FORMAT_PK3` vs `PAKKA_FORMAT_PK4`); the on-disk
bytes, the loader, the writer, and every validation rule are shared.
This doc covers both under one heading; references to "PK3/PK4"
throughout mean "the shared format". The PK3-vs-PK4 split lives
entirely at the dispatch boundary in `src/pakfile.c` — see §6.

Unlike the PAK-class formats covered in
[`quake-pak-format.md`](quake-pak-format.md) and its siblings, PK3 /
PK4 is not a bespoke layout — it is a deliberately constrained subset
of standard ZIP. The canonical reference is PKWARE's APPNOTE.TXT; this
doc covers the subset pakka accepts and the constraints pakka adds on
top.

## 1. Sources

- PKWARE APPNOTE.TXT (current version at
  <https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT>) —
  the canonical ZIP specification. PK3 / PK4 are ZIP, so this is
  the authoritative on-disk reference.
- RFC 1951 (DEFLATE compressed data format) and RFC 1952 (gzip,
  cited only for shared CRC-32 polynomial; pakka does not produce
  gzip streams). PK3 / PK4 DEFLATE payloads are raw RFC 1951
  streams with no zlib wrapper — see §4.
- id Software's GPL'd Quake III Arena source release (2005),
  `code/qcommon/files.c` (`FS_LoadZipFile`) — the reference loader
  for PK3, and the de-facto compatibility target for any tool
  producing PK3s the engine will read.
- id Software's GPL'd Doom 3 source release (2011),
  `neo/framework/File_Manifest.cpp` and `neo/framework/File.cpp` —
  the reference PK4 loader, structurally similar to Q3's.
- pakka's own implementation: `src/pk3file.c` (entire ZIP-class
  read + write pipeline), `src/pakfile.c` (open-time signature
  dispatch + PK3-vs-PK4 label routing), `src/common.h` (`PK3_*`
  size constants, methods, and limits), `src/deflate/` (DEFLATE
  backend abstraction).

The shared loader strategy (PK3 and PK4 being the same format with
different extensions) is documented in `src/pk3file.c`'s opening
comment, and the historical reason — id's `FS_LoadZipFile` was
copied near-verbatim from Q3 to D3 — is preserved by every engine
fork in the wild.

## 2. File layout

A PK3 / PK4 archive is a flat ZIP file with three structural regions
in disk order:

1. **Local File Headers (LFH) + payloads** — one LFH per entry,
   each immediately followed by that entry's compressed-or-stored
   bytes.
2. **Central Directory Records (CDR)** — one 46-byte fixed prefix
   per entry (plus the variable name + extras + comment), grouped
   contiguously after the last payload.
3. **End-of-Central-Directory record (EOCD)** — a single 22-byte
   trailer that points back at the CDR start.

Readers scan backwards from EOF for the EOCD, walk the CDR forward
from there, and seek to each LFH on demand. Writers do the inverse:
stream LFHs + payloads, then emit the CDR, then the EOCD.

### 2.1 EOCD — 22 bytes (fixed prefix, optional comment)

| Offset | Size | Field             | Notes                                          |
| ------ | ---- | ----------------- | ---------------------------------------------- |
| 0      | 4    | signature         | `"PK\x05\x06"`.                                |
| 4      | 2    | disk_num          | u16 LE — must be 0 (no multi-disk).            |
| 6      | 2    | disk_with_cdr     | u16 LE — must be 0.                            |
| 8      | 2    | entries_on_disk   | u16 LE — entry count.                          |
| 10     | 2    | total_entries     | u16 LE — must equal entries_on_disk.           |
| 12     | 4    | cdr_size          | u32 LE — total bytes occupied by the CDR.      |
| 16     | 4    | cdr_offset        | u32 LE — byte offset of the first CDR record.  |
| 20     | 2    | comment_len       | u16 LE — length of trailing comment (0..65535).|

The 22-byte fixed prefix is followed by an optional `comment_len`-byte
comment. Total EOCD region is `22 + comment_len` bytes. ZIP locates
the EOCD by scanning the last 65557 bytes (22 + 65535) for the
signature; pakka does the same in `pk3_find_eocd`.

**ZIP64 sentinels** (`total_entries == 0xFFFF`, `cdr_size ==
0xFFFFFFFF`, `cdr_offset == 0xFFFFFFFF`) are refused at open with
`PAKKA_ERR_UNSUPPORTED`. PK3 / PK4 in the wild never use ZIP64.

### 2.2 CDR record — 46-byte fixed prefix + variable

| Offset | Size | Field            | Notes                                            |
| ------ | ---- | ---------------- | ------------------------------------------------ |
| 0      | 4    | signature        | `"PK\x01\x02"`.                                  |
| 4      | 2    | version_made_by  | u16 LE — pakka writes 20 (DOS / 2.0).            |
| 6      | 2    | version_needed   | u16 LE — pakka writes 20.                        |
| 8      | 2    | flags            | u16 LE — bit 0 (encryption) and bit 3 (data descriptor) refused; pakka writes 0. |
| 10     | 2    | method           | u16 LE — 0 (STORED) or 8 (DEFLATE) only.         |
| 12     | 2    | dos_time         | u16 LE — pakka writes 0.                         |
| 14     | 2    | dos_date         | u16 LE — pakka writes 0x0021 (1980-01-01).       |
| 16     | 4    | crc32            | u32 LE — IEEE 802.3 CRC32 of the *uncompressed* bytes. |
| 20     | 4    | csize            | u32 LE — compressed payload size.                |
| 24     | 4    | usize            | u32 LE — uncompressed payload size.              |
| 28     | 2    | name_len         | u16 LE — bytes of name following the prefix.     |
| 30     | 2    | extra_len        | u16 LE — bytes of extra field.                   |
| 32     | 2    | comment_len      | u16 LE — bytes of per-entry comment.             |
| 34     | 2    | disk_num         | u16 LE — must be 0.                              |
| 36     | 2    | internal_attrs   | u16 LE — pakka writes 0.                         |
| 38     | 4    | external_attrs   | u32 LE — upper 16 bits encode Unix file type (see §3). |
| 42     | 4    | lfh_offset       | u32 LE — file offset of the matching LFH.        |

The prefix is followed by `name_len` filename bytes (no NUL
terminator on disk — length-prefixed), then `extra_len` extra-field
bytes, then `comment_len` per-entry comment bytes. Total record size
is `46 + name_len + extra_len + comment_len`.

### 2.3 LFH — 30-byte fixed prefix + variable

| Offset | Size | Field            | Notes                                          |
| ------ | ---- | ---------------- | ---------------------------------------------- |
| 0      | 4    | signature        | `"PK\x03\x04"`.                                |
| 4      | 2    | version_needed   | u16 LE — pakka writes 20.                      |
| 6      | 2    | flags            | u16 LE — bits 0 and 3 refused.                 |
| 8      | 2    | method           | u16 LE — 0 or 8.                               |
| 10     | 2    | dos_time         | u16 LE.                                        |
| 12     | 2    | dos_date         | u16 LE.                                        |
| 14     | 4    | crc32            | u32 LE.                                        |
| 18     | 4    | csize            | u32 LE.                                        |
| 22     | 4    | usize            | u32 LE.                                        |
| 26     | 2    | name_len         | u16 LE.                                        |
| 28     | 2    | extra_len        | u16 LE.                                        |

Followed by `name_len` filename bytes + `extra_len` extra-field
bytes; the payload (csize bytes of STORED or DEFLATE data) starts
immediately after. There is no per-comment field on the LFH.

The LFH duplicates fields the CDR already records (method, CRC,
sizes, name, extras). This is a ZIP design choice that lets streaming
readers parse one entry at a time without seeking to the CDR. pakka
**cross-checks** every LFH against its CDR record at open time
(`pk3_validate_lfh` in `src/pk3file.c`); any disagreement returns
`PAKKA_ERR_FORMAT`.

### 2.4 Endianness

Every multi-byte field in every record is **little-endian**, same as
the PAK-class formats. pakka's PK3 / PK4 I/O uses local `pk3_read_u16`
/ `pk3_read_u32` / `pk3_put_u16` / `pk3_put_u32` helpers in
`src/pk3file.c` (not the `pakka_read_u32_le` helpers used for PAK)
because the ZIP path operates on in-memory buffers, not stdio
streams. The byte-extraction semantics are identical.

## 3. What pakka accepts and refuses

### 3.1 Compression methods

| Method | Name    | Accepted? | Notes                                  |
| ------ | ------- | --------- | -------------------------------------- |
| 0      | STORED  | yes       | Raw bytes; csize must equal usize.     |
| 8      | DEFLATE | yes       | Raw RFC 1951 (no zlib wrapper). See §4.|
| any other | —    | no        | Refused with `PAKKA_ERR_UNSUPPORTED`. |

This matches what id's `FS_LoadZipFile` accepts in the Q3 / D3
sources; in practice every PK3 / PK4 in the wild uses 0 or 8.

### 3.2 Refusals

The following are refused at open time:

- **Encryption (general-purpose flag bit 0).** No engine PK3 / PK4
  uses ZIP encryption; pakka does not implement decryption.
- **Data descriptors (flag bit 3).** Crucial sizes (csize, usize,
  CRC) are written *after* the payload in this mode, which precludes
  pakka's "validate before reading" model. Refused.
- **ZIP64 (sentinel values 0xFFFF / 0xFFFFFFFF in EOCD or CDR
  fields).** PK3 / PK4 spec is non-ZIP64 by construction.
- **Multi-disk archives** (`disk_num`, `disk_with_cdr`,
  `disk_entries != total_entries`, or a `PK\x07\x08` signature on
  open).
- **Symlinks** — refused when the CDR's `external_attrs` upper 16
  bits encode a Unix `IFLNK` mode (`(ext_attrs >> 16) & 0o170000 ==
  0o120000`). pakka does not create symlinks on extract regardless,
  but rejecting the entry at open time gives a cleaner error than
  "extracted file is the wrong type".
- **Control bytes in entry names** (< 0x20 or == 0x7F), checked
  across the **full declared name length**, not just the C-string
  view. A crafted `"good.txt\0../escape"` would otherwise
  NUL-terminate to `"good.txt"` in memory while the on-disk record
  carries the full path-traversal payload.

### 3.3 Limits

- **Open-time CDR size cap**: 64 MiB (`PK3_MAX_CDR_SIZE`). The
  EOCD's `cdr_size` is a u32 and a hostile archive can declare it
  inflated to force a 4 GiB malloc before per-entry validation
  runs. pakka caps this separately from the per-entry
  `max_decompressed` knob (which the caller sets after `pakka_open`,
  too late to gate the CDR allocation).
- **Per-entry uncompressed payload cap**: configurable via
  `pakka_set_max_decompressed_size`, default 64 MiB. Applies to
  `pakka_open_entry` / `pakka_read_entry_alloc`; STORED entries
  use the declared size directly, DEFLATE entries cap both
  compressed and decompressed allocations against it.
- **Entry count cap on write**: 65535 entries (u16 EOCD field).
  Larger archives need ZIP64, which pakka doesn't emit.
- **Per-entry name length cap**: 255 bytes (one less than
  `PAKKA_ENTRY_NAME_SIZE = 256`), so the in-memory C-string buffer
  always has a `\0` slot. ZIP itself allows 65535-byte names; this
  is a pakka policy.
- **Archive size cap**: 4 GiB (u32 cdr_offset + cdr_size + EOCD
  size). pakka rejects writes that would push the file past
  `UINT32_MAX`.

### 3.4 Directory entries

A ZIP entry whose name ends in `/` and whose `csize` and `usize` are
both zero represents a directory marker. pakka **accepts and silently
skips** these on read (real-world tools like `zip(1)` and 7-Zip emit
them; refusing would break compatibility with archives the engine
loaders happily ignore). On write, pakka never emits them — directory
hierarchy is implicit in the slash-separated entry names.

## 4. DEFLATE — read and write

PK3 / PK4 DEFLATE payloads are **raw RFC 1951 streams** with no
zlib wrapper (no 2-byte zlib header, no Adler-32 trailer). pakka
routes both compress and decompress through the abstraction at
`src/deflate/deflate_iface.h`, which has two interchangeable
backends:

| Backend  | Source                                       | Selection                          |
| -------- | -------------------------------------------- | ---------------------------------- |
| Vendored | `src/vendor/sdefl/` + `src/vendor/sinfl/`    | Default (no system zlib needed).   |
| zlib     | System `<zlib.h>` with `windowBits = -MAX_WBITS` | `make PAKKA_DEFLATE_BACKEND=zlib` / `cmake -DPAKKA_USE_ZLIB=ON`. |

Both backends export the same `pakka_deflate_compress` /
`pakka_deflate_inflate` surface; `src/pk3file.c` never includes
either codec directly.

The negative `windowBits` value on the zlib backend selects raw
DEFLATE (no zlib wrapper) — this is the documented `inflateInit2`
contract from zlib 1.2.0+.

### 4.1 Write-path strategy

Default compression mode for a freshly opened PK3 / PK4 is STORED.
Call `pakka_set_compression(archive, PAKKA_COMPRESSION_DEFLATE, &err)`
(or pass `--compress` on the CLI) to switch new adds to DEFLATE.
The mode is sticky on the in-memory archive handle; it does not
persist across close+reopen.

When DEFLATE is set, `pakka_pk3_add_file_impl` / `_memory_impl`
takes an **eager-capture** branch:

1. Read the source payload entirely into memory.
2. Call `pakka_deflate_compress` with a worst-case-sized output
   buffer.
3. If the encoded output is **strictly smaller** than the source,
   stash the encoded bytes and mark `pk3_method = DEFLATE`.
4. Otherwise (DEFLATE didn't win, or the buffer was too large for
   the backend's per-call size cap), stash the raw bytes and mark
   `pk3_method = STORED`. This is the info-zip auto-fallback
   behaviour; it lets `--compress` work on archives mixing
   compressible text and incompressible PNG / MP3 / pre-compressed
   payloads.

STORED-only adds keep a lighter **streaming source-path** branch
(`pk3_pending_source`) that defers the read until commit time.
DEFLATE adds always buffer the encoded result because the eager
encode already paid the read cost.

The vendored backend caps each compress/inflate call at `INT_MAX`
bytes (sdefl / sinfl take `int`); 2 GiB+ inputs on the add path fall
back to STORED. The zlib backend accepts the full 4 GiB ZIP u32
range.

### 4.2 Read-path

`pakka_open_entry` against a DEFLATE entry allocates a `usize`-byte
output buffer (bounded by `max_decompressed`), reads `csize`
compressed bytes off disk into a separate input buffer, and runs
`pakka_deflate_inflate` to completion. Subsequent `pakka_reader_read`
calls serve from the inflated buffer. STORED entries skip both
buffers and stream `csize == usize` bytes directly via `fread`.

The vendored backend can only report consumed-bytes equal to the
declared csize; trailing garbage inside an entry's declared csize
slips past. The zlib backend reports exact `z.total_in` and catches
this. The CRC32 cross-check in `pakka_pk3_deep_verify_entry` is the
end-to-end integrity backstop for both backends.

## 5. CRC32

PK3 / PK4 entries carry an IEEE 802.3 CRC32 (polynomial 0xEDB88320,
reflected) over the **uncompressed** bytes. pakka:

- **On read**: stashes the CRC from the CDR. Does **not** verify
  it during normal extract — payloads are read for content, not
  integrity, and the LFH/CDR cross-check already validates the
  metadata.
- **On `pakka_verify` deep mode**: streams the decompressed bytes
  through `pakka_pk3_deep_verify_entry`, recomputes the CRC, and
  compares against the declared value. Mismatch is a verify-time
  error.
- **On write**: computes CRC over the uncompressed source as the
  payload is read, before encoding, and writes the same value
  into the LFH and CDR.

The CRC table (`pk3_crc32_table[256]`) lives in `src/pk3file.c` as a
file-scope constant; no first-use construction.

## 6. PK3 vs PK4 in pakka

The two labels diverge in exactly three places:

1. **`pakka_open` signature dispatch** (`src/pakfile.c`): a ZIP
   magic (`PK\x03\x04` for LFH or `PK\x05\x06` for empty EOCD)
   triggers `pakka_zip_format_from_path()` to pick PK3 vs PK4 from
   the filename extension. The format hint can override the
   extension; mismatched-hint vs magic returns
   `PAKKA_ERR_INVALID_ARGUMENT`.
2. **`pakka_create` format gate** (`src/pakfile.c`): the public
   `pakka_create(path, format, ...)` validates that `format` is one
   of the supported labels and stamps `pak->format` before tail-
   calling the shared `pakka_pk3_create_impl`.
3. **CLI extension sniffer** (`src/cli.c`): `.pk3` → PK3, `.pk4` →
   PK4 for create-time format selection. `--format pk3` / `--format
   pk4` overrides the extension.

Every other ZIP-class dispatch — add, commit, open_entry, reader_read,
verify, the load-directory skip — gates on `pakka_format_is_zip()`
from `src/common.h` so both labels share one branch. The
implementation in `src/pk3file.c` never reads `pak->format`; the
caller owns the label.

PK4 is what Doom 3 (2004) and Doom 3 BFG Edition (2012) ship; PK3 is
what Quake III Arena (1999), Quake III Team Arena, Return to Castle
Wolfenstein, Wolfenstein: Enemy Territory, Jedi Knight II, Jedi
Academy, Soldier of Fortune II, and every Q3-engine fork
(ioquake3, OpenArena, Tremulous, World of Padman, ...) ship. The
PK4 label is recognisable in the wild by file extension only.

## 7. Cross-format notes

Where the PAK-class formats sit in a tight 3-row family
(`PACK` 56/64, `SPAK` 120/128, `PACK` 56/72), PK3 / PK4 sits on a
different axis entirely — it is the ZIP container, and pakka
intentionally implements only the subset id's engines actually
emit:

- No timestamps (DOS date/time zeroed to a sentinel).
- No per-entry comments.
- No extra fields written (extras tolerated on read).
- STORED + DEFLATE only.
- Non-ZIP64.

This is enough for every shipping Q3 / D3 PK3 / PK4 and every
modding tool's output (Q3 demos, dhewm3 mod paks, ioquake3 pure
servers). The constrained-subset stance is documented in id's
`FS_LoadZipFile` and inherited verbatim.

## 8. Test coverage

- `tests/pk3.bats` — end-to-end CLI tests against synthetic PK3
  archives covering STORED + DEFLATE entries, LFH/CDR/EOCD
  validation, encryption / data-descriptor / ZIP64 / multi-disk
  refusals, symlink rejection, control-byte name rejection,
  directory-marker tolerance, mixed STORED+DEFLATE write, and
  rebuild-after-delete.
- `tests/pk4.bats` — the same suite re-run with `.pk4` extensions
  + `--format pk4`, asserting that the bytes are identical to the
  PK3 case and the only observable difference is the label
  returned by `pakka_format()`.
- `tests/pk3_q3demo.bats` — opt-in suite (`make realpak-test-q3`) that
  downloads the Quake III Arena shareware demo from archive.org,
  uses pakka to extract the inner `pak0.pk3` (a 47 MiB real-world
  id-built PK3 with 1,274 entries), and exercises list / extract /
  verify against it. The hard fixture pin keeps the suite honest
  against authentic id-emitted bytes.
- `tests/c_api_test.c` — public-API round-trip for both PK3 and
  PK4, including the PK3 ↔ PK4 label round-trip through
  `pakka_create` + `pakka_open`.
- `linux-glibc-x86_64-zlib` CI job — full bats suite against the
  zlib DEFLATE backend, catches divergence between the vendored
  and zlib codecs.
- `linux-glibc-s390x-be` CI job — same suite under QEMU big-endian
  s390x, catches byte-order regressions in the local
  `pk3_read_*` / `pk3_put_*` helpers.

## 9. Related docs

- [`quake-pak-format.md`](quake-pak-format.md) — Quake / Q2 /
  GoldSrc PAK (the PK3 ancestor).
- [`sin-pak-format.md`](sin-pak-format.md) — SiN's wider PAK row.
- [`daikatana-pak-format.md`](daikatana-pak-format.md) — Daikatana's
  custom-codec PAK row.
