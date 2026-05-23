# Quake / Quake II / GoldSrc PAK format

A complete reference for the vanilla `PACK` archive format used by
Quake (id Software, 1996), Quake II (id Software, 1997), and the
GoldSrc engine that Valve forked from Quake (Half-Life 1996/1998,
Counter-Strike 1.6, Team Fortress Classic, Sven Co-op, Ricochet, ...).

All three engines read and write the same on-disk format: a 12-byte
header followed by a contiguous run of payload bytes and a trailing
64-byte-per-entry directory. There is no per-entry compression, no
CRC, no timestamps, no permissions — the format is a flat name → byte
range table. Two close relatives — SiN's `SPAK` and Daikatana's wider
`PACK` row — are covered in §6.

## 1. Sources

References that informed this document:

- id Software's GPL'd Quake source release (1999),
  `WinQuake/common.c` (`COM_LoadPackFile`) and `qfiles.h`
  (`dpackheader_t` / `dpackfile_t`) — the authoritative reference
  implementation. The 56-byte filename field and 64-byte entry size
  are baked into those struct definitions, and every later engine
  with PAK support reads them the same way.
- id Software's Quake II source release (2001),
  `qcommon/files.c` (`FS_LoadPackFile`) — identical layout,
  unchanged constants. Q2 simply forked the loader.
- Valve's GoldSrc engine, derived from Quake — the PAK loader was
  preserved bit-for-bit when Valve forked, and Half-Life 1's
  `valve/pak0.pak` parses cleanly with id's loader.
- "The Unofficial Quake Specs" (UQS) by Olivier Montanuy (1996),
  §3 "PACK files" — early community spec, useful for confirming
  the layout matched what the engine actually wrote.
- Quake Wiki — <https://quakewiki.org/wiki/.pak> — community
  reference covering all three engines together.
- pakka's own implementation: `src/pakfile.c` (header parse, directory
  load, write paths), `src/common.c` (PAK-class geometry table),
  `src/common.h` (constants + struct layout).

The format is small enough that all sources agree to the byte; this
document is the consolidated reference pakka uses.

## 2. File layout

### 2.1 Header — 12 bytes

| Offset | Size | Field       | Notes                                          |
| ------ | ---- | ----------- | ---------------------------------------------- |
| 0      | 4    | `signature` | `"PACK"` (no NUL).                             |
| 4      | 4    | `diroffset` | u32 LE — byte offset of the directory block.   |
| 8      | 4    | `dirlength` | u32 LE — directory size in bytes (64 × N).     |

`dirlength` is the directory size in **bytes**, not entries; entry
count is `dirlength / 64`. The directory can sit anywhere in the
file. id's own `pak0.pak` ships with the directory mid-file and an
orphan `progs.dat` past it in a hole left by an in-place recompile —
loaders cannot assume the directory is at EOF, nor that entries are
listed in byte-layout order.

`diroffset` must be ≥ 12 (cannot point inside the header). pakka
also rejects archives whose `diroffset + dirlength` extends past the
on-disk file size; see `load_pakfile` in `src/pakfile.c`.

### 2.2 Directory entry — 64 bytes

| Offset | Size | Field         | Notes                                                |
| ------ | ---- | ------------- | ---------------------------------------------------- |
| 0      | 56   | `filename`    | NUL-padded path; forward-slash separators.           |
| 56     | 4    | `file_pos`    | u32 LE — offset of payload bytes in the archive.     |
| 60     | 4    | `file_length` | u32 LE — payload size in bytes.                      |

Total: 64 bytes per entry. id's `qfiles.h` declares this as
`dpackfile_t { char name[56]; int filepos, filelen; }`.

**Filename field — 56 bytes.** Originally intended as a `char[56]`
holding a NUL-terminated path. In practice the field is treated as
opaque bytes by the engine and any byte past the terminator is
ignored; some archives in the wild fail to zero-pad. A robust loader
must treat the field as 56 bytes that may or may not contain a NUL —
if the first 56 bytes have no terminator, the name is exactly 56
chars long. pakka materialises this by reading 56 bytes into a
57-byte buffer and unconditionally writing `'\0'` at index 56 before
any `strlen` / `strcmp` / `printf("%s", ...)` downstream.

**Path separator.** Forward slash. Every shipping PAK from id and
Valve uses `/`; the loaders also accept it without normalisation on
Windows because the Quake VFS layer was written on NeXTSTEP/IRIX and
inherited POSIX paths. Embedded directory components are common
(`maps/e1m1.bsp`, `sound/items/damage1.wav`); the format itself does
not store directory entries — the path string is the entire
hierarchy.

**No NUL hygiene guarantee from writers.** id's `dpackfile_t` is a
plain struct written via raw `fwrite`. The 56 bytes after the
terminator are whatever stack garbage happened to be there. A
loader-level NUL-clamp is mandatory if `filename` will be passed to
C string APIs. pakka's `load_directory` enforces this; see
`src/pakfile.c:869`.

**Sizes.** Both `file_pos` and `file_length` are unsigned 32-bit
little-endian, so the maximum archive size is 4 GiB and the maximum
entry size is 4 GiB. The Quake engine declared them as plain `int`
(signed); negative values are not a defined case — pakka treats them
as unsigned per the modern convention and rejects entries whose
`file_pos + file_length` extends past the file.

### 2.3 Endianness and host portability

Every on-disk u32 (`diroffset`, `dirlength`, per-entry `file_pos` and
`file_length`) is **little-endian regardless of host byte order**.
Quake originated on x86 / NeXTSTEP-on-x86 and the format encodes
that. Engine ports to big-endian targets (PowerPC Mac OS, SGI IRIX,
later s390x / ppc64be Linux) byte-swap on load.

In pakka the directory I/O sites use `pakka_read_u32_le` /
`pakka_write_u32_le` from `src/common.c`; the CI matrix includes a
QEMU-emulated big-endian s390x job to keep this honest. Never
`fread` / `fwrite` a `uint32_t` directly against a pak.

### 2.4 No compression

The vanilla `PACK` format has no compression, no encryption, no
checksums, no extra fields. Payloads are stored byte-for-byte. The
only metadata is name + offset + length.

This makes the format trivially mmap-friendly (the engine maps the
whole archive once and serves entries as `(base + file_pos,
file_length)` ranges) at the cost of zero compression-ratio savings.
Quake 3 moved to ZIP (PK3) precisely for that — see `dev/docs/`
sibling docs when they exist.

### 2.5 No directory ordering invariant

The directory entries can appear in any order, and the payloads they
point at can appear in any byte order within the file. id's
`pak0.pak` famously has entries listed in alphabetical-ish order but
payloads scattered out of sequence, with a 410,616-byte orphan
`progs.dat` past the directory left over from an in-place recompile.

This has three consequences for any tool that walks the format:

1. **Don't assume the directory sits at EOF.** Find the highest live
   `file_pos + file_length` across all entries to find the true
   payload tail. pakka's `compute_payload_end` does this.
2. **Don't assume payloads are sequential.** Entries can overlap (a
   shared `palette.lmp`, say) or leave holes. pakka does not
   produce overlapping entries on write, but it accepts them on
   read — the format permits it.
3. **Don't shift bytes in place on delete.** A delete can leave a
   hole that subsequent adds may or may not fill. pakka's
   `pakka_delete` + `pakka_commit` rewrites the archive through a
   temp file in that case rather than risk corrupting an
   out-of-order layout.

## 3. Engine variants sharing the row

The 56/64 row is shared by:

| Engine                     | Examples of archives parsed by the same loader              |
| -------------------------- | ----------------------------------------------------------- |
| Quake 1 (id, 1996)         | `pak0.pak`, `pak1.pak`                                      |
| Quake II (id, 1997)        | `pak0.pak` through `pak3.pak`                               |
| GoldSrc / Half-Life 1      | `valve/pak0.pak`, `cstrike/pak0.pak`, mod paks              |
| Half-Life mods             | TFC, Counter-Strike 1.6, Day of Defeat, Sven Co-op, ...     |
| Ritual / Hexen II / etc.   | Any engine forked from Quake 1 source                       |

There are no documented per-engine extensions to the 64-byte entry —
the differences across this family are in the **payloads**, not the
container. Q1 stores `.mdl` / `.bsp` / `.wav` / `.lmp`; Q2 stores
`.md2` / `.bsp` / `.wav` / `.pcx` / `.wal`; GoldSrc stores `.mdl`
(v10 — different format from Q1's `.mdl`) / `.bsp` (v30) /
`.wav` / `.spr`. The PAK container does not care.

In pakka the `pak` / `goldsrc` / `hl` `--format` aliases all map to
the same `PAKKA_FORMAT_PAK` enum value, which selects the 56/64
geometry row. There is no functional split.

## 4. I/O integration in pakka

### 4.1 Reading

`pakka_open` reads the 4-byte signature and dispatches on magic:
`"PACK"` enters the PAK-class path (this format and Daikatana share
the magic — see §6), `"SPAK"` enters the SiN path, `"PK\x03\x04"` /
`"PK\x05\x06"` enter the ZIP-class path. The PAK-class path then
runs an open-time layout probe to disambiguate Quake's 64-byte rows
from Daikatana's 72-byte rows by validating every entry's
`file_pos + file_length` against the file size under each candidate
geometry; for vanilla Quake / Q2 / GoldSrc the 64-byte layout wins.

After format resolution, `load_directory` reads every entry, NUL-clamps
the filename, validates the offset / length range against the
captured `file_size`, and chains entries into a singly-linked list
rooted at `archive->head`. Payload reads happen lazily via
`pakka_open_entry` / `pakka_reader_read`, which seek to
`entry->offset` and stream `entry->length` bytes.

### 4.2 Writing

`pakka_create(path, PAKKA_FORMAT_PAK, ...)` opens a temp file and
stamps a 12-byte `PACK` header with `dirlength = 0`. Subsequent
`pakka_add_file` / `pakka_add_memory` calls stream the payload to
`tail->offset + tail->length` and append an entry to the in-memory
list with the new name + offset + length. Commit modes:

- **Add-only.** `pakka_commit` (also called implicitly by
  `pakka_close`) writes the directory at byte-offset
  `tail->offset + tail->length` and rewrites the header in place.
  No payload bytes move.
- **Rebuild.** Any `pakka_delete` forces a rebuild via a scratch
  temp: surviving entries get copied (their raw bytes streamed
  through `copy_between_paks`), a fresh directory is written, and
  the scratch is renamed over the original. The 4 GiB ceiling
  (`diroffset + dirlength ≤ UINT32_MAX`) is checked before any write.

Filename validation on add applies the same name-safety check used at
extract time (no `..` components, no leading `/`, no Windows-reserved
device names, no control bytes) and rejects names longer than 55
bytes (the cap is 55, not 56, so the on-disk field always has a
trailing NUL even though the loader doesn't require one).

### 4.3 Verify

`pakka_verify` walks every entry, runs the name-safety check, streams
the payload to confirm `file_pos` and `file_length` point at
readable bytes, and flags portable-union collisions on extraction
(case fold, slash/backslash, trailing dot/space). There is no
checksum to deep-verify against; the verify pass is structural only.
The CLI's `--verify` mode threads this through.

## 5. Edge cases the format permits

- **Empty archive.** `dirlength = 0`, `diroffset = 12` is a valid
  empty pak. pakka produces this when every entry has been deleted;
  it is rewritten as a bare 12-byte `PACK\0\0\0\f\0\0\0\0` header.
- **Empty entry.** `file_length = 0` is valid. The entry has a name
  but no payload bytes; `pakka_open_entry` returns an empty reader.
- **Duplicate names.** The format does not prohibit two entries with
  the same name. Engine behaviour is "first wins" or "last wins"
  depending on which side of the loader's hashtable insertion order
  the duplicates land. pakka's `pakka_find_entry` returns the first
  match in linked-list order, which equals directory order, which
  equals on-disk order for archives pakka wrote. Verify flags
  duplicates as warnings.
- **Name longer than 55 bytes.** The on-disk field is 56 bytes with
  no required terminator; pakka caps adds at 55 bytes so the field
  always NUL-terminates, but tolerates exactly-56-byte names on
  read.
- **Out-of-order or overlapping payloads.** Permitted by the format;
  pakka reads them but never writes them.
- **Orphan bytes.** Bytes outside any entry's `[file_pos,
  file_pos+file_length)` range, and outside the
  `[diroffset, diroffset+dirlength)` range, are not referenced by
  the directory and are ignored on read. id's `pak0.pak` ships with
  a 410 KiB orphan `progs.dat` in this category. pakka's rebuild
  path drops orphans on the next commit.

## 6. Cross-format notes

The PAK row sits in a family of three closely related layouts that
pakka enumerates side by side in `src/common.c`:

```
                   signature  name_field_len  dir_entry_size  has_compression
Quake / Q2 / HL     "PACK"            56               64             no
SiN                 "SPAK"           120              128             no
Daikatana           "PACK"            56               72            yes
```

- **SiN** (Ritual Entertainment, 1998) widens the filename field to
  120 bytes and the directory entry to 128 bytes; the header layout
  and offset/length semantics are otherwise identical. The
  `"SPAK"` magic disambiguates it at open time.
- **Daikatana** (Ion Storm, 2000) extends each entry with two
  trailing u32s (`compressed_length` + `is_compressed`) and a
  custom LZSS-style byte-codec. The full reference is at
  `dev/docs/daikatana-pak-format.md`. Because DK shares Quake's
  `"PACK"` magic, the open-time layout probe in `src/pakfile.c`
  disambiguates 64- vs 72-byte entries by validating every entry's
  on-disk extent against the file size under each candidate row.

Every PAK-class read/write site in pakka dispatches off the
`pakka_pak_geometry(fmt)` table rather than hard-coding constants, so
adding a future variant means appending a row, not forking every
code path.

## 7. Test coverage

- `test/pakka.bats` — end-to-end CLI tests against id's shareware
  `pak0.pak` (downloaded + SHA-verified by `make test`). Covers
  list, extract, create, add, delete, verify, and the
  out-of-order / orphan-byte cases id's archive exhibits.
- `test/c_api_test.c` — public API round-trip: `pakka_create` for
  PAK + `pakka_add_memory` + `pakka_close` + `pakka_open` +
  `pakka_read_entry_alloc` + memcmp.
- `test/large_file_test.c` — exercises a 2.42 GiB sparse pak to
  catch 32-bit-`long` truncation bugs in the seek/tell path.
- `linux-glibc-s390x-be` CI job — runs the full test suite under
  QEMU-emulated big-endian s390x to catch byte-order regressions
  in `pakka_read_u32_le` / `pakka_write_u32_le`.

The Quake / Q2 / GoldSrc row is the most heavily exercised format in
the suite; SiN and Daikatana share most of the same code paths via
the geometry table but have their own targeted tests
(`test/sin_test.c`, `test/dk_test.c`).
