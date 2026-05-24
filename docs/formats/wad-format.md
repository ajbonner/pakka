# Doom WAD format (IWAD / PWAD)

A complete reference for the Doom 1 / 2 WAD ("Where's All the Data")
archive format as implemented by pakka.

WAD is the structural ancestor of the Quake PAK family: a 12-byte
header followed by a flat directory of fixed-size entries pointing at
opaque payload bytes. The format diverges from Quake-style PAK in three
places that matter for parsing and authoring — header field order,
directory entry field order, and an explicit allowance for duplicate
lump names. The two label variants `IWAD` (id-shipped base data) and
`PWAD` (modder patch) share every byte except the 4-byte magic; pakka
treats them as label variants of one impl, mirroring the PK3 / PK4
split.

## 1. Sources

References that informed this document:

- John Carmack et al., `linuxdoom-1.10` source release (id Software,
  1997, GPLv2). `w_wad.h` declares the on-disk `wadinfo_t` (header) and
  `filelump_t` (directory entry) structs; `w_wad.c` implements the
  loader and the per-lump fetch path. Authoritative spec.
- Doom Wiki, "WAD" — <https://doomwiki.org/wiki/WAD>. Useful for the
  modder-facing conventions: marker lumps, namespace pairs (`F_START` /
  `F_END`, `S_START` / `S_END`, ...), per-map lump grouping.
- Freedoom — <https://freedoom.github.io/>. BSD-licensed IWADs
  (`freedoom1.wad`, `freedoom2.wad`) usable as redistributable real-
  world test fixtures.
- pakka's own implementation: `src/pakfile.c` (header read/write, dir
  read/write, duplicate-name policy), `src/common.c` (PAK-class
  geometry table, IWAD/PWAD rows).

## 2. File layout

### 2.1 Header — 12 bytes

| Offset | Size | Field           | Notes                                                                  |
| ------ | ---- | --------------- | ---------------------------------------------------------------------- |
| 0      | 4    | `signature`     | `"IWAD"` or `"PWAD"`; pakka detects each at open time and stamps both at create. |
| 4      | 4    | `numlumps`      | int32 LE — directory entry **count** (not bytes).                      |
| 8      | 4    | `infotableofs`  | int32 LE — byte offset of the directory block in the archive.          |

Two differences from the Quake / SiN / Daikatana header that share this
12-byte shape:

1. **Field order is reversed.** Quake-class stores `(diroffset,
   dirlength)`; WAD stores `(numlumps, infotableofs)`. pakka normalizes
   the in-memory representation to a single `(diroffset, dirlength)`
   pair regardless of format; the WAD field-order branch lives in
   `load_pakfile` (read) and `write_pak_header` (write).
2. **The count field is in entries, not bytes.** A WAD with 4 lumps has
   `numlumps = 4`; a Quake PAK with 4 entries has `dirlength = 4 * 64 =
   256`. pakka synthesizes `dirlength_bytes = numlumps * 16` on load,
   and derives `numlumps = dirlength_bytes / 16` on write.

Both fields are little-endian (Doom shipped on DOS/x86). They're
declared `int32` in id's source but always populated with non-negative
values; pakka reads them as `uint32_t` consistently with the rest of
the PAK-class loaders and lets the bounds check at load time catch any
wrapped value.

### 2.2 Directory entry — 16 bytes

| Offset | Size | Field      | Notes                                                                                 |
| ------ | ---- | ---------- | ------------------------------------------------------------------------------------- |
| 0      | 4    | `filepos`  | int32 LE — payload offset in the archive.                                             |
| 4      | 4    | `size`     | int32 LE — payload size in bytes (uncompressed; WAD has no compression).              |
| 8      | 8    | `name`     | 8-byte ASCII lump name, NUL-padded if shorter, **no trailing NUL** when name is exactly 8 bytes. |

Field order is the opposite of every other PAK-class format: `(filepos,
size, name)` instead of `(name, offset, length)`. pakka contains this
in `load_directory` (read) and `write_pak_entry` (write) via the same
`pakka_format_is_wad()` branch used by the header serializers.

Names are conventionally uppercase ASCII with no path separators —
lumps are addressed by 8-character ID. Common Doom lumps that occupy
the full 8 bytes include `COLORMAP`, `TEXTURE1`, `TEXTURE2`,
`LINEDEFS`, `SIDEDEFS`, `SSECTORS`, `BLOCKMAP`. pakka accepts any
ASCII byte; engines uppercase on lookup.

### 2.3 Endianness, limits, and signed-vs-unsigned

- Every u32 on disk is little-endian. WAD predates the use of any
  big-endian id-engine target.
- pakka caps `numlumps` at the shared `PAKFILE_MAX_ENTRIES`
  (1,048,576). The synthesized `dirlength = numlumps * 16` therefore
  fits in u32 with margin (16 MiB max).
- The wire format is u32-bounded at 4 GiB. pakka rejects WADs whose
  `infotableofs + dirlength_bytes` would cross that ceiling, matching
  the existing PAK / SiN / DK guards.
- id's source declares header and directory fields as `int32_t`.
  Negative values would be invalid for both `filepos` and `size`; pakka
  treats them as `uint32_t` and lets the bounds check (`extent >
  file_size` and `offset > file_size - extent`) catch the wrap.

## 3. IWAD vs PWAD

Two magic values; one byte layout. Engine semantics:

- **IWAD** ("Internal WAD") — base data the engine boots from. id's
  shipped `doom.wad`, `doom2.wad`, `tnt.wad`, `plutonia.wad`. Contains
  every lump the engine needs to run on its own.
- **PWAD** ("Patch WAD") — overlay loaded on top of an IWAD. Modder
  output. The engine merges PWAD lumps with the IWAD's: name lookups
  scan the directory backwards from end, so PWAD lumps shadow same-
  named IWAD lumps without overwriting them.

pakka treats this distinction as a label, not a separate format. The
`pakka_format_t` enum carries two values (`PAKKA_FORMAT_IWAD`,
`PAKKA_FORMAT_PWAD`) but the same internal read/write code services
both — the 4-byte magic comes from the geometry row, which is the only
on-disk difference. This mirrors the PK3 / PK4 split (Quake 3 vs Doom 3
ZIP archives, byte-identical layout, different label).

CLI defaults: the `.wad` extension defaults to PWAD on create (modder
authoring is the common case). IWAD targets need an explicit `--format
iwad`. There is no `wad` alias for `--format` — IWAD and PWAD are not
bit-identical, so picking one would be misleading.

## 4. Marker lumps

Doom uses zero-byte entries as namespace delimiters. The directory
entries are real (they occupy 16 bytes each, advance `numlumps`, and
appear in linked-list / index traversals), but the engine never reads
the named bytes — `size` is zero, so there's nothing to read.

Common marker names:

- `F_START` / `F_END` — flat (floor / ceiling) namespace boundary.
- `FF_START` / `FF_END` — extended flat namespace used by PWADs (some
  ports recognize both pairs).
- `P_START` / `P_END` — patch (wall) namespace; sometimes split into
  `P1_START` / `P1_END`, `P2_START` / `P2_END`, `P3_START` / `P3_END`.
- `S_START` / `S_END` — sprite namespace.
- `SS_START` / `SS_END` — extended sprite namespace for PWAD overlays.

Because `size == 0`, `filepos` carries no meaning — the engine never
fetches the bytes. In practice marker lumps ship with `filepos == 0`,
and pakka relaxes the usual "offset must be ≥ header size" check
specifically for zero-size WAD entries so these load cleanly. The
upper-bound check (`offset > file_size`) still catches garbage.

## 5. Duplicate names

Doom maps deliberately reuse lump names. Every map (`E1M1`, `MAP01`,
...) is followed by ten fixed-name lumps in directory order: `THINGS`,
`LINEDEFS`, `SIDEDEFS`, `VERTEXES`, `SEGS`, `SSECTORS`, `NODES`,
`SECTORS`, `REJECT`, `BLOCKMAP`. `doom2.wad` ships 32 maps and
therefore 32 copies of each of those names. The engine walks the
directory positionally and uses the surrounding map-marker lump to
group them.

pakka's policy:

- **Open:** `validate_no_duplicates` early-returns OK for WAD. The
  archive loads with all duplicates intact.
- **Find by name:** `pakka_find_entry` returns the first match in
  directory order. This differs from engine semantics (engines search
  backwards), but pakka stays byte-faithful: we report what's on disk
  in the order it's on disk, not what an engine would resolve.
- **Iterate by index:** `pakka_entry_at(archive, i)` is the canonical
  way to walk a duplicate-bearing WAD. Combined with
  `pakka_open_entry_handle(archive, entry, ...)` this gives full
  read-side access to every duplicate without ambiguity.
- **Extract by name (CLI):** selects only the first match per path
  argument. Selecting all matches would collide on disk for any
  destination filesystem that doesn't tolerate duplicate filenames in a
  single directory.
- **Extract all (CLI):** refuses on duplicate-bearing WADs via the
  existing normalized-collision check, which fires for two identical
  names. Power users with a duplicate-bearing WAD should use the C API
  (`pakka_entry_at` + `pakka_open_entry_handle`) to extract by index.
- **Add (CLI / C API):** duplicate names are allowed on WAD; every
  other format still rejects with `PAKKA_ERR_DUPLICATE`.
- **Delete:** removes the first match. Mass-delete-by-name on a WAD
  with many duplicates requires repeated `-d` calls.
- **Verify:** duplicate findings are reported as `PAKKA_REPORT_WARNING`
  on WAD (legal authoring pattern) and `PAKKA_REPORT_ERROR` on every
  other format.

## 6. pakka integration notes

The WAD-aware branches in the codebase live at five sites in
`src/pakfile.c`, each gated by `pakka_format_is_wad()`:

| Site                            | Purpose                                                                            |
| ------------------------------- | ---------------------------------------------------------------------------------- |
| `load_pakfile` magic ladder     | Detect IWAD / PWAD magic alongside SPAK and PACK; validate format hint.            |
| `load_pakfile` header read      | Read `(numlumps, infotableofs)` instead of `(diroffset, dirlength)`; synthesize the byte-oriented in-memory pair. |
| `load_pakfile` PACK / DK probe  | Positive list (`PAK` or `DAIKATANA`) excludes WAD from the layout probe.           |
| `load_directory` entry read     | Read `(filepos, size, name)` instead of `(name, offset, length)`.                  |
| `load_directory` offset check   | Allow `filepos < header_size` when `size == 0` (marker lumps).                     |
| `validate_no_duplicates`        | Early-return OK; WADs ship duplicates by design.                                   |
| `pak_add_preflight`             | Loosen the name-length cap (8 ASCII bytes with no NUL, vs `name_len < name_cap`); skip the duplicate-name check. |
| `write_pak_header`              | Emit `(numlumps, infotableofs)`; takes an `op_name` parameter for create vs commit diagnostics. |
| `write_pak_entry`               | Emit `(filepos, size, name)`.                                                      |

The geometry table (`src/common.c::pakka_pak_geometry_rows`) gains two
rows for IWAD and PWAD with `name_field_len = 8`,
`dir_entry_size = 16`, `has_compression = 0`. No new fields on the
geometry struct — the WAD divergences are localized enough that adding
a `pakka_format_is_wad()` helper plus the nine branch sites above is
clearer than gluing on extra geometry flags.

The CLI in `src/cli.c` recognises `.wad` (case-insensitive) as PWAD on
create, accepts `iwad` and `pwad` as `--format` tokens, and applies
WAD-aware first-match selection in `op_extract`'s path-matching loop.
The reopen step in `op_extract` uses `pakka_open_entry_handle` rather
than name lookup so duplicate-bearing WADs read coherently entry-by-
entry.

Testing: `test/wad_test.c` covers 20 synthetic-fixture scenarios
(header byte layout, directory entry byte layout, 8-char name
acceptance, marker lump loading, duplicate-name handling, extract-by-
name first-match semantics, extract-all collision refusal, rebuild
round-trip, format hint mismatches, AUTO open without probe ambiguity,
numlumps overflow, garbage `filepos` bounds check, verify). The C-API
exerciser in `test/c_api_test.c` adds an IWAD / PWAD label round-trip
(create with each label, confirm the on-disk magic, reopen and confirm
`pakka_format()` reports the requested label, exercise the duplicate-
name allowance via `pakka_add_memory`).

No real-world WAD fixtures ship in default CI — Freedoom is the
recommended manual stress fixture for anyone wanting to verify against
a multi-thousand-lump IWAD.
