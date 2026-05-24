# PK3 / PK4 format

A reference for the `.pk3` (Quake 3, 1999) and `.pk4` (Doom 3, 2004)
archive formats as implemented by pakka.

PK3 and PK4 are **byte-identical ZIP containers**. The only thing the
filename extension changes is the `pakka_format_t` label the caller
sees back (`PAKKA_FORMAT_PK3` vs `PAKKA_FORMAT_PK4`); the on-disk
bytes, the loader, the writer, and every validation rule are shared.
This doc covers both under one heading; references to "PK3/PK4"
throughout mean "the shared format". The PK3-vs-PK4 split lives
entirely at the dispatch boundary in `src/pakfile.c` — see §7.

Unlike the PAK-class formats covered in
[`quake-pak-format.md`](quake-pak-format.md) and its siblings, PK3 /
PK4 is not a bespoke layout — it is a deliberately constrained subset
of standard ZIP. The canonical reference is PKWARE's APPNOTE.TXT; this
doc covers the subset pakka accepts and the constraints pakka adds on
top.

## 1. Sources

### 1.1 Primary references

- PKWARE APPNOTE.TXT — the canonical ZIP specification. PK3 / PK4
  are ZIP, so this is the authoritative on-disk reference. Current
  version: <https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT>.
  Version-pinned mirror at the Library of Congress for v6.3.3 (the
  edition pakka's spec citations match):
  <https://www.loc.gov/preservation/digital/formats/digformatspecs/APPNOTE(20120901)_Version_6.3.3.txt>.
  Historical version list:
  <https://pkware.cachefly.net/webdocs/APPNOTE/> (6.3.0 through the
  current point release).
- ISO/IEC 21320-1:2015 — *Information technology — Document
  Container File — Part 1: Core*.
  <https://www.iso.org/standard/60101.html>. A normative,
  intentionally constrained ZIP profile that prohibits encryption,
  digital signatures, patched data, multi-volume archives, and any
  compression method other than `0` (STORED) and `8` (DEFLATE) — the
  same subset pakka enforces in §3. Implementations conforming to
  ISO/IEC 21320-1 round-trip cleanly with pakka's writer.
- RFC 1951 (DEFLATE compressed data format,
  <https://www.rfc-editor.org/rfc/rfc1951>) and RFC 1952 (gzip,
  <https://www.rfc-editor.org/rfc/rfc1952>, cited only for shared
  CRC-32 polynomial; pakka does not produce gzip streams). PK3 / PK4
  DEFLATE payloads are raw RFC 1951 streams with no zlib wrapper —
  see §4. RFC 1950 (zlib wrapper format,
  <https://www.rfc-editor.org/rfc/rfc1950>) is cited only to clarify
  what PK3 / PK4 *do not* carry.
- id Software's GPL'd Quake III Arena source release (2005),
  [`code/qcommon/files.c`](https://github.com/id-Software/Quake-III-Arena/blob/master/code/qcommon/files.c)
  (`FS_LoadZipFile`) — the reference loader for PK3, and the
  de-facto compatibility target for any tool producing PK3s the
  engine will read.
- id Software's GPL'd Doom 3 source release (2011),
  [`neo/framework/FileSystem.cpp`](https://github.com/id-Software/DOOM-3/blob/master/neo/framework/FileSystem.cpp) —
  the reference PK4 loader, structurally similar to Q3's. The Doom 3
  BFG Edition source adds the resource-pack derivative at
  [`neo/framework/File_Resource.cpp`](https://github.com/id-Software/DOOM-3-BFG/blob/master/neo/framework/File_Resource.cpp);
  pakka does not target the BFG resource container, only the PK4
  archives the BFG executable still consumes alongside.
- pakka's own implementation: `src/pk3file.c` (entire ZIP-class
  read + write pipeline), `src/pakfile.c` (open-time signature
  dispatch + PK3-vs-PK4 label routing), `src/common.h` (`PK3_*`
  size constants, methods, and limits), `src/deflate/` (DEFLATE
  backend abstraction).

### 1.2 Additional references

- ioquake3 —
  [`code/qcommon/files.c`](https://github.com/ioquake/ioq3/blob/main/code/qcommon/files.c).
  Maintained Q3 source port; the de-facto modern compatibility target
  for PK3-emitting tools.
- dhewm3 —
  [`neo/framework/FileSystem.cpp`](https://github.com/dhewm/dhewm3/blob/master/neo/framework/FileSystem.cpp).
  Maintained Doom 3 source port; modern PK4 compatibility target.
- libzip — <https://libzip.org/documentation/libzip.html>. Mature
  read/write ZIP library (C, BSD-3-Clause); reference for current
  UTF-8 / CP437 handling, atomic-write semantics, and Info-ZIP-style
  extra-field interpretation.
- Info-ZIP — <https://infozip.sourceforge.net/>. The de-facto CLI
  reference implementation (`unzip` / `zip`); behaviour for the
  legacy methods and the original CP437 default.
- Kaitai Struct formal `.ksy` grammar: <https://formats.kaitai.io/zip/>
  (source:
  [`archive/zip.ksy`](https://github.com/kaitai-io/kaitai_struct_formats/blob/master/archive/zip.ksy)) —
  machine-readable ZIP spec with generated parsers in many languages.
- Library of Congress Sustainability of Digital Formats,
  "ZIP File Format":
  <https://www.loc.gov/preservation/digital/formats/fdd/fdd000354.shtml>
  — preservation-oriented summary of ZIP versions and interoperability.
- Wikipedia, "ZIP (file format)" —
  <https://en.wikipedia.org/wiki/ZIP_(file_format)>. Useful as a
  cross-cited tertiary summary covering method numbers, ZIP64
  history, and the Implode / Shrink / Reduce legacy methods pakka
  does not implement.
- hanshq, "Shrink, Reduce, and Implode: The Legacy Zip Compression
  Methods" — <https://www.hanshq.net/zip2.html>. Annotated reference
  for the methods PK3 / PK4 never use; useful background when
  triaging an archive pakka refuses with `PAKKA_ERR_UNSUPPORTED`.

The shared loader strategy (PK3 and PK4 being the same format with
different extensions) is documented in `src/pk3file.c`'s opening
comment, and the historical reason — id's `FS_LoadZipFile` was
copied near-verbatim from Q3 to D3 — is preserved by every engine
fork in the wild.

### 1.3 Preservation

Every live external reference in §1.1 and §1.2 was submitted to the
[Wayback Machine](https://web.archive.org/) on 2026-05-24. To fetch
a snapshot of any link above, prepend `https://web.archive.org/web/`
to its URL. Sources that carry their own archival guarantee (RFCs,
ISO standards, GitHub source files, Wikipedia, the Library of
Congress) are excluded from the submission set.

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

### 2.5 ZIP record signature catalog

Every record in a ZIP stream begins with a 4-byte magic. The full
APPNOTE.TXT §4.3 record set, with pakka's per-record handling:

| Signature      | Record                                       | pakka handling                                                                                       |
| -------------- | -------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `PK\x03\x04`   | Local File Header (LFH)                      | Written, parsed, cross-checked against CDR (§2.3). Also one of the open-time format-dispatch magics. |
| `PK\x01\x02`   | Central Directory Record (CDR)               | Written, parsed per-record (`pk3_validate_cdr_record`). Wrong magic mid-CDR → "Bad CDR signature at entry N". |
| `PK\x05\x06`   | End of Central Directory (EOCD)              | Written; located by reverse scan over the last 65557 bytes (`pk3_find_eocd`). Also a format-dispatch magic for empty archives. |
| `PK\x07\x08`   | Optional Data Descriptor / spanning marker   | Refused at open: as the leading 4 bytes it indicates a multi-disk archive; as the post-payload data descriptor it requires general-purpose flag bit 3, which is rejected (`PAKKA_ERR_UNSUPPORTED`). |
| `PK\x06\x06`   | ZIP64 End of Central Directory               | Not searched for. ZIP64 is detected upstream via `0xFFFF`/`0xFFFFFFFF` sentinels in the EOCD (refused with `"ZIP64 archives are not supported"`) or per-entry CDR (`"ZIP64 per-entry fields are not supported"`); the ZIP64 EOCD record itself is never reached. |
| `PK\x06\x07`   | ZIP64 EOCD Locator                           | Same — never searched for; the sentinel check upstream refuses before this record matters.           |
| `PK\x06\x08`   | Archive Extra Data Record                    | Not searched for. If present between LFHs / CDRs, would surface as a "Bad CDR signature" or LFH-validation error. |
| `PK\x05\x05`   | Digital signature                            | Not searched for. Lies between the CDR and EOCD; harmless to the EOCD reverse scan, and unreferenced by anything pakka reads. The signature bytes are silently ignored on read and never emitted on write. |
| `PK\x30\x30`   | Temporary spanning marker                    | Not specifically recognized. If it appears as the leading 4 bytes of a file, the signature ladder in `pakka_open` falls through to `"Not a pak file (bad signature)"`; if it appears later in a stream pakka is parsing, the surrounding record-signature check rejects it. pakka has no spanning code path. |

For the upstream record catalog see APPNOTE.TXT §4.3
(<https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT>).

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

### 3.5 General-purpose flag bits — pakka's stance

The 16-bit `flags` word appears in both the LFH (offset 6) and the
CDR (offset 8). APPNOTE.TXT §4.4.4 defines the full set; pakka's
stance per bit:

| Bit | Mask     | APPNOTE meaning                                 | pakka                                                                                  |
| --- | -------- | ----------------------------------------------- | -------------------------------------------------------------------------------------- |
| 0   | `0x0001` | Encrypted entry                                 | Refused at open (`PAKKA_ERR_UNSUPPORTED`, "Encrypted ZIP entries are not supported"). pakka writes 0. |
| 1   | `0x0002` | DEFLATE level high (method 8) / 8K window (method 6) | Ignored on read (informational); pakka writes 0.                                   |
| 2   | `0x0004` | DEFLATE level low / 4K window                   | Ignored on read; pakka writes 0.                                                       |
| 3   | `0x0008` | Data descriptor follows payload                 | Refused at open (`PAKKA_ERR_UNSUPPORTED`, "ZIP data descriptors are not supported"). pakka writes 0. |
| 4   | `0x0010` | Enhanced DEFLATE                                | Ignored on read; pakka writes 0.                                                       |
| 5   | `0x0020` | Compressed patched data                         | Not actively refused; never seen in PK3 / PK4. pakka writes 0.                         |
| 6   | `0x0040` | Strong encryption                               | Not actively checked. APPNOTE requires bit 0 also set for strong-encryption archives, and pakka refuses bit 0; an archive that set bit 6 alone (no shipping tool does) would be accepted as-is. pakka writes 0. |
| 7-10 | —       | Unused / reserved                               | Ignored. pakka writes 0.                                                               |
| 11  | `0x0800` | Language encoding flag (EFS) — entry name + comment are UTF-8 | Read: prefer UTF-8 decode (§5 of [windows-codepage](windows-codepage.md)); GP-bit-11-set + non-UTF-8 bytes = `PAKKA_ERR_FORMAT`. Write: pakka sets the bit when the name has any byte > 0x7F **and** is valid UTF-8 per RFC 3629; pure-ASCII names leave it clear. |
| 12  | `0x1000` | Reserved for PKWARE enhanced compression        | Ignored. pakka writes 0.                                                               |
| 13  | `0x2000` | Encrypted central directory                     | Not actively checked. An encrypted CDR would also encrypt entries (bit 0), which pakka refuses; pakka does not inspect bit 13 independently. pakka writes 0. |
| 14-15 | —      | Reserved                                        | Ignored. pakka writes 0.                                                               |

The "Ignored" rows reflect that pakka does not enforce or interpret
those bits beyond writing zero on commit. None of them appear in
shipping PK3 / PK4 archives.

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

The polynomial is the IEEE 802.3 CRC-32 (`0xEDB88320` LSB-first,
`0x04C11DB7` MSB-first), shared with PNG, gzip, and Ethernet. The
canonical reference is in RFC 1952 §8 and ITU-T V.42 §8.

## 6. Security profile

PK3 / PK4 archives often come from untrusted sources (mod sites,
multiplayer pure-server downloads, archived demos). The treatment
below is the worked-through threat model behind the refusals in §3.2
and the per-entry caps in §3.3.

### 6.1 Decompression bombs

A small DEFLATE payload can declare a 4 GiB `usize`. pakka caps both
the on-disk CDR allocation and the per-entry decompressed buffer:

- `PK3_MAX_CDR_SIZE` (default 64 MiB) bounds the EOCD-declared
  `cdr_size` before any CDR bytes are read. A hostile archive
  declaring `cdr_size = 0xFFFFFFFF` is rejected at the malloc gate.
- `pakka_set_max_decompressed_size` (default 64 MiB) bounds
  `pakka_open_entry` / `pakka_read_entry_alloc`. STORED entries use
  the declared `csize` (which equals `usize`); DEFLATE entries cap
  both the compressed and decompressed allocations against the
  setting. The CRC32 cross-check (§5) catches mid-stream truncation
  but does not bound peak RSS — the cap does.

### 6.2 Path traversal

The entry-name safety check (`pakka_unsafe_entry_name` in
`src/common.c`) rejects `..` path components, absolute paths
(leading `/`), drive-letter prefixes, Windows reserved device names
(`CON`, `PRN`, `AUX`, `NUL`, `COM1..9`, `LPT1..9`), and any control
byte (`< 0x20` or `== 0x7F`). The check runs across the **full
declared `name_len`**, not the C-string view, so a crafted
`"good.txt\0../escape"` cannot smuggle a traversal through a NUL.

### 6.3 Symlinks and special file types

The CDR's `external_attrs` upper 16 bits encode a Unix mode word on
archives written by Info-ZIP and friends. pakka rejects entries
whose mode encodes `IFLNK` (`(ext_attrs >> 16) & 0o170000 ==
0o120000`) at open time. The extract path never calls `symlink(2)`
regardless, so an undetected symlink would materialize as a regular
file with the link target as its contents — annoying but not unsafe.
The open-time rejection is the cleaner diagnostic.

### 6.4 LFH / CDR disagreement

The LFH duplicates fields the CDR records (§2.3). Crafted archives
in the wild use this to lie to streaming readers (which trust the
LFH) while presenting a sanitized CDR (which random-access readers
trust). pakka cross-checks `method`, `crc32`, `csize`, `usize`,
`name_len`, and the name bytes themselves at open time
(`pk3_validate_lfh`); any disagreement returns `PAKKA_ERR_FORMAT`.

### 6.5 Overlapping payloads

A pathological archive can declare two CDR entries pointing at
overlapping LFH ranges. pakka does not currently reject this — the
read paths seek to each LFH independently — but every LFH offset is
bounds-checked against the file size, so an overlap cannot read past
EOF. Overlap is flagged as a verify-time warning by `pakka_verify`
when the per-entry on-disk extents intersect.

### 6.6 Name-encoding ambiguity

See [`windows-codepage.md`](windows-codepage.md) §5 for the
UTF-8-versus-CP437 decode policy and the false-positive caveat in
§5.3 (some CP1251 byte sequences are accidentally legal UTF-8). PK3
/ PK4 has no encoding-flag-clears mechanism beyond GP bit 11; pakka
silently prefers UTF-8 on read.

### 6.7 Out of scope

- **Hardlink-like ZIP entries** (two entries pointing at the same
  LFH offset by design) — not seen in PK3 / PK4 in the wild; pakka
  treats them as overlap (§6.5).
- **Polyglot files** (an archive that is also a valid PNG / PDF /
  ELF). pakka reads from the EOCD reverse scan, so prefix garbage
  is tolerated by construction — including up to 65557 bytes of
  arbitrary prefix that a polyglot could carry.
- **Quine ZIPs** (archives that contain themselves). pakka reads,
  never recursively expands; safe by absence-of-feature.

## 7. PK3 vs PK4 in pakka

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

## 8. Cross-format notes

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

## 9. Test coverage

- `test/pk3_test.c` — end-to-end CLI tests against synthetic PK3
  archives covering STORED + DEFLATE entries, LFH/CDR/EOCD
  validation, encryption / data-descriptor / ZIP64 / multi-disk
  refusals, symlink rejection, control-byte name rejection,
  directory-marker tolerance, mixed STORED+DEFLATE write, and
  rebuild-after-delete. Also drives the c-API harness in-process
  (max_decompressed cap, commit-refuses-changed-source,
  open_entry_handle paths, rebuild rollback in-memory) and the
  fault-injection cases via subprocess respawn.
- `test/pk4_test.c` — the same shape re-run with `.pk4` extensions
  + `--format pk4`, asserting that the bytes are identical to the
  PK3 case and the only observable difference is the label
  returned by `pakka_format()`.
- `test/pk3_q3demo_test.c` — opt-in suite (`make realpak-test-q3`)
  that downloads the Quake III Arena shareware demo from
  archive.org, uses pakka to extract the inner `pak0.pk3` (a 47 MiB
  real-world id-built PK3 with 1,274 entries), and exercises list /
  extract / verify against it. The hard fixture pin keeps the suite
  honest against authentic id-emitted bytes.
- `test/c_api_test.c` — public-API round-trip for both PK3 and
  PK4, including the PK3 ↔ PK4 label round-trip through
  `pakka_create` + `pakka_open`.
- `linux-glibc-x86_64-zlib` CI job — full C test suite against the
  zlib DEFLATE backend, catches divergence between the vendored
  and zlib codecs.
- `linux-glibc-s390x-be` CI job — same suite under QEMU big-endian
  s390x, catches byte-order regressions in the local
  `pk3_read_*` / `pk3_put_*` helpers.

## 10. Related docs

- [`quake-pak-format.md`](quake-pak-format.md) — Quake / Q2 /
  GoldSrc PAK (the PK3 ancestor).
- [`sin-pak-format.md`](sin-pak-format.md) — SiN's wider PAK row.
- [`daikatana-pak-format.md`](daikatana-pak-format.md) — Daikatana's
  custom-codec PAK row.
