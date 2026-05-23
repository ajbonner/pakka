# SiN PAK format

A reference for the `SPAK` archive format used by SiN (Ritual
Entertainment, 1998) and its mission pack *Wages of Sin* (2WS).

SiN's pak format is a direct widening of the Quake / Q2 PAK layout
documented in [`quake-pak-format.md`](quake-pak-format.md). Every
structural rule from that doc (12-byte header, on-disk LE u32s,
unordered directory, no compression, no checksums, NUL-padded
filenames without a required terminator, orphan-bytes tolerance,
empty-archive shape, the `pakka_read_u32_le` / `pakka_write_u32_le`
contract) applies unchanged. This doc covers only the differences.

## 1. Sources

- Ritual Entertainment's shipped retail and demo archives — the
  authoritative reference. SiN's `pak0.sin` / `pak1.sin` are the
  byte corpus the format derives from.
- yquake2/pakextract — <https://github.com/yquake2/pakextract>
  (BSD-2-Clause). Reference C reader for the broader Quake-PAK
  family including SiN.
- pakka's own implementation: `src/pakfile.c` (signature dispatch,
  geometry-driven directory I/O), `src/common.c` (PAK-class geometry
  table — the SiN row sits next to PAK and DK), `src/common.h`
  (`PAKKA_ENTRY_NAME_SIZE` 256-byte cap that accommodates SiN's
  wider field).

There is no published SiN format spec; the layout is consistent
across every shipping archive and the three values in the geometry
row are the entire spec.

## 2. Differences from Quake / Q2 PAK

Three values change vs the 56/64 row in `quake-pak-format.md` §2:

| Field            | Quake / Q2 / GoldSrc | SiN     |
| ---------------- | -------------------- | ------- |
| Signature magic  | `"PACK"`             | `"SPAK"`|
| Filename field   | 56 bytes             | 120 bytes |
| Directory entry  | 64 bytes total       | 128 bytes total |

Everything else is identical:

- Header layout: 4-byte signature + u32 LE `diroffset` + u32 LE
  `dirlength`, total 12 bytes.
- Per-entry fields after the filename: u32 LE `file_pos` + u32 LE
  `file_length`, same offsets-from-filename-end as Quake (just
  shifted by 64 bytes within the entry because the name field is
  wider).
- No compression, no CRC, no flags, no timestamps.
- Forward-slash path separators.
- Same orphan-bytes / out-of-order-payload tolerance.

### 2.1 Header — 12 bytes

| Offset | Size | Field       | Notes                                          |
| ------ | ---- | ----------- | ---------------------------------------------- |
| 0      | 4    | `signature` | `"SPAK"` (no NUL).                             |
| 4      | 4    | `diroffset` | u32 LE — byte offset of the directory block.   |
| 8      | 4    | `dirlength` | u32 LE — directory size in bytes (128 × N).    |

`dirlength` divides by **128**, not 64. Entry count is
`dirlength / 128`.

### 2.2 Directory entry — 128 bytes

| Offset | Size | Field         | Notes                                                |
| ------ | ---- | ------------- | ---------------------------------------------------- |
| 0      | 120  | `filename`    | NUL-padded path; forward-slash separators.           |
| 120    | 4    | `file_pos`    | u32 LE — offset of payload bytes in the archive.     |
| 124    | 4    | `file_length` | u32 LE — payload size in bytes.                      |

The 64-byte widening compared to Quake is entirely in the filename
field: SiN allocates 120 bytes where Quake allocates 56. The NUL-pad
convention and "may run the full 120 bytes with no terminator" caveat
from the Quake doc apply identically — the in-memory buffer is sized
one byte larger so a `'\0'` can be force-written at index 120 before
any C string API touches it.

The wider name was Ritual's response to deeper directory hierarchies
than Quake originally needed. SiN content paths like
`pics/skins/aphrodite_redteam.tga` already crowd Quake's 56-byte
limit; 120 bytes was enough headroom for the entire shipping corpus.

## 3. Disambiguation

Unlike Daikatana, SiN's `"SPAK"` magic is unique within the PAK-class
family and never collides with another row. pakka's `pakka_open`
dispatches `"SPAK"` directly to the SiN geometry without running a
layout probe; `--format sin` / `PAKKA_FORMAT_SIN` skip the probe
explicitly. The open-time signature check rejects a mismatched hint
(`--format pak` on a `"SPAK"` archive, or vice versa) with
`PAKKA_ERR_INVALID_ARGUMENT`.

## 4. I/O integration in pakka

The geometry table in `src/common.c` is the only place SiN-specific
constants live:

```
                   signature  name_field_len  dir_entry_size  has_compression
SiN                 "SPAK"           120              128             no
```

Every PAK-class read/write site — `load_directory`, `pakka_add_file`
/ `_memory`'s name-cap check, `write_pak_entry`, `write_pak_directory`,
`init_pak_header` — consults `pakka_pak_geometry(fmt)` and uses the
returned row's values. No SiN code path is forked; the same loop
that reads a Quake entry reads a SiN entry, just with `geom->name_field_len`
= 120 and `geom->dir_entry_size` = 128.

The `write_pak_entry` scratch buffer is sized to `PAKKA_ENTRY_NAME_SIZE`
(256 bytes), which accommodates SiN's 120 without dynamic stack
allocation. This is the reason `PAKKA_ENTRY_NAME_SIZE` exists as a
separate constant from `PAKFILE_PATH_BUF` (57 bytes for the Quake row).

`pakka_create(path, PAKKA_FORMAT_SIN, ...)` stamps a 12-byte `SPAK`
header with `dirlength = 0`; subsequent adds and the commit path
behave identically to PAK (see [`quake-pak-format.md`](quake-pak-format.md)
§4.2). The CLI's extension sniffer maps `.sin` to PAKKA_FORMAT_SIN;
`--format sin` overrides the extension.

Name validation on add caps entry names at 119 bytes (one less than
the on-disk field width, so the field always NUL-terminates), uses
the same `pakka_unsafe_entry_name` traversal check as PAK, and reads
through the same `pakka_normalize_entry_name` for the verify-side
collision check.

## 5. Cross-format notes

The PAK-class family pakka enumerates side by side:

```
                   signature  name_field_len  dir_entry_size  has_compression
Quake / Q2 / HL     "PACK"            56               64             no
SiN                 "SPAK"           120              128             no
Daikatana           "PACK"            56               72            yes
```

See [`quake-pak-format.md`](quake-pak-format.md) §6 for the table's
broader context, and [`daikatana-pak-format.md`](daikatana-pak-format.md)
for the third row.

## 6. Test coverage

- `test/sin.bats` — end-to-end CLI tests against synthetic SiN
  archives. Built by the bats setup with `python3` helpers because
  there is no redistributable retail SiN corpus to ship as a fixture
  (the format is exercised against bytes the test itself writes).
  Covers list, extract, create, add, delete, verify, and the
  name-field-width-specific edge cases (exactly-120-byte names,
  119-byte names + NUL terminator).
- `test/c_api_test.c` — `pakka_create` for SiN + `pakka_add_memory`
  + `pakka_close` + `pakka_open` + `pakka_read_entry_alloc` + memcmp,
  parallel to the PAK / DK / PK3 / PK4 cases in the same exerciser.
- `linux-glibc-s390x-be` CI job — the same byte-order coverage that
  catches PAK regressions catches SiN regressions, because the LE u32
  reads/writes share the `pakka_read_u32_le` / `pakka_write_u32_le`
  helpers.
