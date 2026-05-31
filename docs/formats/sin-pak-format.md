# SiN PAK format

A reference for the `SPAK` archive format used by SiN (Ritual
Entertainment, 1998) and its mission pack *Wages of Sin* (2WS).

SiN's pak format is a direct widening of the Quake / Q2 PAK layout
documented in [`quake-pak-format.md`](quake-pak-format.md). Every
structural rule from that doc (12-byte header, on-disk LE u32s,
unordered directory, no compression, no checksums, NUL-padded
filenames without a required terminator, orphan-bytes tolerance,
empty-archive handling, the `pakka_read_u32_le` / `pakka_write_u32_le`
contract) applies unchanged. This doc covers only the differences.

## 1. Sources

### 1.1 Primary references

- Ritual Entertainment's shipped retail and demo archives — the
  authoritative byte corpus. SiN's `pak0.sin` / `pak1.sin` are the
  reference fixtures the format derives from. Ritual never released
  the engine source through an official GPL-style channel; the
  archived 1998 source dump linked in §1.2 is the only first-party
  loader available, and §2 below has been cross-checked against both
  the shipping corpus and that loader.
- yquake2/pakextract — reader implementation at
  [`pakextract.c`](https://github.com/yquake2/pakextract/blob/master/pakextract.c)
  (BSD-2-Clause). Reference C reader for the broader Quake-PAK
  family including SiN.
- pakka's own implementation: `src/pakfile.c` (signature dispatch,
  geometry-driven directory I/O), `src/common.c` (PAK-class geometry
  table — the SiN row sits next to PAK and DK), `src/common.h`
  (`PAKKA_ENTRY_NAME_SIZE` 256-byte cap that accommodates SiN's
  wider field).

### 1.2 Additional references

- Internet Archive SiN 1998 source dump —
  <https://archive.org/details/si-n-ritual-entertainment-1998-source-code.-7z>.
  Mirrored Ritual engine source covering the SPAK loader — the only
  first-party reference for SiN's pak handling in existence.
- Slartibarty/PAKExtract — <https://github.com/Slartibarty/PAKExtract>.
  Alternative cross-format C reader covering Quake / Q2 / Half-Life /
  SiN. Useful divergence point when sanity-checking pakka's behaviour
  against a second independent implementation.
- wattostudios/GameExtractor —
  <https://github.com/wattostudios/GameExtractor>. Cross-format
  archive reader / writer (Java, GPL) including a SiN SPAK plugin;
  useful as an independent reader for differential testing.
- erysdren, "idTech PAK Format" (2024) — <https://erysdren.me/docs/pak/>.
  Modern consolidated byte-level reference that places SiN alongside
  Quake and HROT under one document.

The layout is consistent across every shipping archive and the three
values in the geometry row are the entire spec.

### 1.3 Preservation

Every live external reference in §1.1 and §1.2 was submitted to the
[Wayback Machine](https://web.archive.org/) on 2026-05-24. To fetch
a snapshot of any link above, prepend `https://web.archive.org/web/`
to its URL. Sources that carry their own archival guarantee (GitHub
source files, Internet Archive item pages) are excluded from the
submission set.

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

The wider name and the higher entry-count cap (see §2.3) landed
late in SiN's development — both bumped on the same day,
October 8, 1998, per the engine source's revision history, about a
month before SiN shipped on November 9, 1998. That timing suggests
Ritual hit Q2's `MAX_PAK_FILENAME_LENGTH` (56) and
`MAX_FILES_IN_PACK` (4,096) ceilings during final asset assembly
rather than designing the divergence up front. The widened name in
particular ended up at exactly 120 bytes — enough headroom for the
deepest paths the shipping corpus needed (SiN content paths like
`pics/skins/aphrodite_redteam.tga` already crowd Quake's 56-byte
limit) without much slack to spare.

### 2.3 Engine-level loading constraints

These are properties of the shipping SiN engine's loader, not of the
SPAK container. Archives that violate them remain format-conformant;
the engine just refuses to mount them.

**Entry count cap.** Ritual's loader caps `numpackfiles` at 16,384
(`MAX_FILES_IN_PACK` in the engine's `q_files.h`) — four times Q2's
4,096. Pakka's own `PAKFILE_MAX_ENTRIES` is 1,048,576, so pakka will
write archives that exceed the engine's cap; shipping `pak0.sin` /
`pak1.sin` are well under it.

**Mac port shipped `.rez`, not `.sin`.** The Mac SiN port (Rebecca
Heineman's Burgerlib build, ca. 2002) replaced the SPAK loader with
Burgerlib's `RezFile` resource container and shipped `pak0.rez` /
`pak1.rez` instead. The on-disk SPAK layout in §2 is preserved as
dead code inside that port's filesystem source but is not the active
read path. PC SiN reads `.sin` files using the layout documented
here; pakka models only the PC path.

**Demo anti-modification check.** The SiN shareware demo's loader
(compiled with a `NO_ADDONS` flag that retail builds disable)
computes a checksum over the directory block — `dirlen` bytes
starting at `diroffset`, payloads excluded — and refuses the
archive if the result diverges from a hardcoded constant. Effect:
any rebuilt demo `pak0.sin` fails to mount on the demo even when
its payload bytes are byte-identical to the original, because the
rebuilt directory's exact byte layout (entry order, slack,
padding) rarely reproduces the original's. Retail SiN omits the
check entirely and accepts any format-valid pak.

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

- `test/sin_test.c` — end-to-end CLI tests against synthetic SiN
  archives. Built by an inline SPAK writer because there is no
  redistributable retail SiN corpus to ship as a fixture (the format
  is exercised against bytes the test itself writes). Covers list,
  extract, create, add, delete, verify, and the
  name-field-width-specific edge cases (exactly-120-byte names,
  119-byte names + NUL terminator).
- `test/c_api_test.c` — `pakka_create` for SiN + `pakka_add_memory`
  + `pakka_close` + `pakka_open` + `pakka_read_entry_alloc` + memcmp,
  parallel to the PAK / DK / PK3 / PK4 cases in the same exerciser.
- `linux-glibc-s390x-be` CI job — the same byte-order coverage that
  catches PAK regressions catches SiN regressions, because the LE u32
  reads/writes share the `pakka_read_u32_le` / `pakka_write_u32_le`
  helpers.
