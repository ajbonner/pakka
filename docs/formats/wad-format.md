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

### 1.1 Primary references

- John Carmack et al., `linuxdoom-1.10` source release (id Software,
  1997, GPLv2).
  [`w_wad.h`](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.h)
  declares the on-disk `wadinfo_t` (header) and `filelump_t` (directory
  entry) structs;
  [`w_wad.c`](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c)
  implements the loader and the per-lump fetch path. Authoritative
  spec.
- Matt Fell, "The Unofficial Doom Specs" v1.666 (1994) — the
  community spec that documented WAD from the outside and made every
  early Doom editing tool possible. Plain text:
  <https://www.gamers.org/dEngine/doom/spec/uds.1666.txt>. The
  HTML-rendered EDGE mirror is at
  <https://edge.sourceforge.net/edit_guide/doom_specs.htm>; a
  Markdown reformat is maintained at
  <https://github.com/nukeop/TheUnofficialDoomSpecs>. The byte-level
  description of the 12-byte header and 16-byte `filelump_t` in §2
  of this doc is in lockstep with Fell's chapter 2.
- Doom Wiki ("DoomWiki.org" — the canonical wiki), "WAD" —
  <https://doomwiki.org/wiki/WAD>. Used here as the modder-facing
  reference for marker lumps, namespace pairs (`F_START` / `F_END`,
  `S_START` / `S_END`, ...), and per-map lump grouping. The Fandom
  mirror at <https://doom.fandom.com/wiki/WAD> is older and less
  curated; prefer the doomwiki.org URL.
- ZDoom Wiki, "WAD" — <https://zdoom.org/wiki/WAD>. Source-port
  perspective; documents the post-`linuxdoom-1.10` marker extensions
  (`TX_START`, `HI_START`, ...) that vanilla Doom never used but
  every modern port accepts. See §4.1.
- Freedoom — <https://freedoom.github.io/>. BSD-licensed IWADs
  (`freedoom1.wad`, `freedoom2.wad`) usable as redistributable real-
  world test fixtures.
- pakka's own implementation: `src/pakfile.c` (header read/write, dir
  read/write, duplicate-name policy), `src/common.c` (PAK-class
  geometry table, IWAD/PWAD rows).

### 1.2 Additional references

- Kaitai Struct formal `.ksy` grammar:
  <https://formats.kaitai.io/doom_wad/> — machine-readable spec
  covering header, directory, marker lumps, and the common map-lump
  payload structs. Generated parsers in Python, C++, Java, JavaScript,
  Ruby, Go, Lua, Perl, Rust.
- ModdingWiki Shikadi, "WAD Format" —
  <https://moddingwiki.shikadi.net/wiki/WAD_Format>. Concise
  cross-game format reference (also covers WAD usage in Rise of the
  Triad and the Hexen / Heretic / Strife corpus).
- Just Solve The File Format Problem, "Doom WAD" —
  <http://fileformats.archiveteam.org/wiki/Doom_WAD>. Preservation-
  focused summary with pointers to historical WAD2 / WAD3 cousins.
- Chocolate Doom — <https://github.com/chocolate-doom/chocolate-doom>.
  Historically accurate Doom source port; its
  [`src/w_wad.c`](https://github.com/chocolate-doom/chocolate-doom/blob/master/src/w_wad.c)
  is a modernized rewrite of id's loader that preserves the
  byte-faithful behaviour and is the de-facto cross-check target for
  WAD readers.
- DSDA-Doom — <https://github.com/kraflab/dsda-doom>. PrBoom+ derived
  source port; useful divergence point for demo-compatible WAD
  handling.
- DeuTex / DeuSF — <https://github.com/Doom-Utils/deutex>. WAD
  composer for Doom / Heretic / Hexen / Strife; the canonical text
  format directives for emitting a WAD from a directory tree.
- SLADE 3 — <https://github.com/sirjuddington/SLADE>. Modern
  cross-platform WAD / PK3 editor that has effectively superseded
  XWE and SlumpEd; useful as a manual inspection tool when triaging
  oddities in a real-world fixture.
- Omgifol — <https://github.com/devinacker/omgifol>. Python WAD
  library; a high-level reference implementation useful for
  cross-validating reader output against a different programming
  language and idiom.

The 12-byte header and 16-byte entry layout are simple enough that
all sources agree to the byte; this document is the consolidated
reference pakka uses.

### 1.3 Preservation

Every live external reference in §1.1 and §1.2 was submitted to the
[Wayback Machine](https://web.archive.org/) on 2026-05-24. To fetch
a snapshot of any link above, prepend `https://web.archive.org/web/`
to its URL. Sources that carry their own archival guarantee (GitHub
source files, ArchiveTeam's Just Solve wiki) are excluded from the
submission set.

Two pages block Save Page Now and have no Wayback snapshot:
[`zdoom.org/wiki/Special_lumps`](https://zdoom.org/wiki/Special_lumps)
(linked from §4.1) and
[`formats.kaitai.io/doom_wad/`](https://formats.kaitai.io/doom_wad/)
(linked from §1.2). Stable substitutes:
[`kaitai-io/kaitai_struct_formats/game/doom_wad.ksy`](https://github.com/kaitai-io/kaitai_struct_formats/blob/master/game/doom_wad.ksy)
is the underlying grammar that kaitai.io renders, and DoomWiki.org's
per-marker pages cover the same material as the ZDoom Special Lumps
catalog.

## 2. File layout

### 2.1 Header — 12 bytes

| Offset | Size | Field           | Notes                                                                  |
| ------ | ---- | --------------- | ---------------------------------------------------------------------- |
| 0      | 4    | `signature`     | `"IWAD"` or `"PWAD"`; pakka detects each at open time and stamps both at create. |
| 4      | 4    | `numlumps`      | int32 LE — directory entry **count** (not bytes).                      |
| 8      | 4    | `infotableofs`  | int32 LE — byte offset of the directory block in the archive.          |

Two differences from the Quake / SiN / Daikatana header that share this
12-byte layout:

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

### 4.1 Source-port marker extensions

Beyond the `linuxdoom-1.10` set, modern Doom source ports add their
own marker pairs. The container format is unchanged — these are still
zero-size 16-byte directory entries — but the names are new and only
recognized by ports that opt in. pakka treats these as ordinary
entries: the names round-trip byte-for-byte and the duplicate-name
policy in §5 still applies.

| Pair                       | Origin               | Purpose                                                                          |
| -------------------------- | -------------------- | -------------------------------------------------------------------------------- |
| `TX_START` / `TX_END`      | ZDoom (2003)         | Stand-alone textures (lumps in PNG or Doom patch format) outside `TEXTURE1/2`.   |
| `HI_START` / `HI_END`      | GZDoom (2006)        | High-resolution texture replacements; scaled to match the corresponding non-HI lump. |
| `VX_START` / `VX_END`      | EDGE                 | Voxel models.                                                                    |
| `C_START` / `C_END`        | EDGE / Vavoom        | Colormaps.                                                                       |

Source-port wikis are the canonical reference here:
<https://zdoom.org/wiki/WAD> and the per-marker pages
<https://doomwiki.org/wiki/TX_START>,
<https://doomwiki.org/wiki/HI_START>. The
[ZDoom "Special lumps" page](https://zdoom.org/wiki/Special_lumps)
covers the broader set of well-known lump names a port may interpret.

### 4.2 Map lump grouping

Doom maps are encoded as a *positional* group of lumps following a
map-marker lump (`ExMy` for Doom 1, `MAPnn` for Doom 2). The map
marker is itself a zero-size lump; the engine identifies a map by
seeing one of the map markers and consuming the next *N* lumps in
directory order as that map's payload. Names within the group can
duplicate across maps (§5).

pakka does not interpret map-lump payloads — to pakka they are
opaque bytes inside a duplicate-tolerant container. The grouping is
documented here only because correct authoring depends on emitting
the lumps in the right order; emit a map's `LINEDEFS` before its
`SIDEDEFS` and the engine reads garbage.

Vanilla Doom (`linuxdoom-1.10`) — 10 lumps per map, fixed order:

| Order | Lump        | Contents                                                  |
| ----- | ----------- | --------------------------------------------------------- |
| 0     | `THINGS`    | Map objects (player starts, monsters, items, decorations).|
| 1     | `LINEDEFS`  | Line definitions (walls between sectors).                 |
| 2     | `SIDEDEFS`  | Sidedef texture / offset data, one per linedef side.      |
| 3     | `VERTEXES`  | 2D vertex coordinates.                                    |
| 4     | `SEGS`      | BSP-cut line segments.                                    |
| 5     | `SSECTORS`  | Subsectors (convex polygon leaves of the BSP).            |
| 6     | `NODES`     | BSP tree internal nodes.                                  |
| 7     | `SECTORS`   | Sector definitions (floor / ceiling height + textures).   |
| 8     | `REJECT`    | Sector-to-sector visibility bitmap (used by enemy AI).    |
| 9     | `BLOCKMAP`  | 128×128 collision-detection acceleration grid.            |

Hexen / ZDoom — 11 lumps per map, adds `BEHAVIOR` between `SECTORS`
and `REJECT` (compiled ACS bytecode for the Hexen scripting system).

UDMF (Universal Doom Map Format) — uses a different layout entirely:
a `TEXTMAP` lump (UTF-8 text describing geometry) bracketed by
`ZNODES`, optional `REJECT` / `DIALOGUE` / `BEHAVIOR`, and closed by
an `ENDMAP` marker lump. Source: ZDoom Wiki,
<https://zdoom.org/wiki/Universal_Doom_Map_Format>.

GL nodes (separate BSP tree usable by hardware-accelerated ports) —
adds `GL_VERT` / `GL_SEGS` / `GL_SSECT` / `GL_NODES` either inline
after the regular map lumps or in a separate `gl_<mapname>.wad`.
Reference: <https://zdoom.org/wiki/Node>.

The shipped maps in Freedoom and the Doom shareware IWAD follow the
vanilla 10-lump layout; pakka's test fixtures synthesize a single
map in vanilla order to exercise the duplicate-name handling without
depending on a downloaded IWAD.

### 4.3 WAD2 / WAD3 — unrelated formats sharing the name

Quake (1996) and GoldSrc (1998) ship a different "WAD" format used
for textures only:

- **WAD2** — Quake's `gfx.wad` and similar texture archives. Magic
  `"WAD2"`, 12-byte header `(magic, numlumps, infotableofs)`, 32-byte
  directory entries with 16-byte names and a per-lump `type` /
  `compression` byte. The container shares the WAD name but is
  byte-incompatible with Doom WAD.
- **WAD3** — GoldSrc's texture WAD (`halflife.wad`, `liquids.wad`,
  ...). Magic `"WAD3"`, same overall layout as WAD2 with miptex payload
  changes. Half-Life 1's texture pipeline reads WAD3; the GoldSrc PAK
  carries everything else.

pakka does **not** read or write WAD2 or WAD3. They are documented
here only to remove the ambiguity that the shared name creates. See
the Just Solve File Format Problem page
<http://fileformats.archiveteam.org/wiki/Quake_WAD> for the WAD2
layout, and the
[Valve Developer Community "WAD" page](https://developer.valvesoftware.com/wiki/WAD)
for WAD3 specifics.

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

The WAD-aware branches in the codebase live at nine sites in
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
