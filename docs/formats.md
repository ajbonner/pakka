# Format support

Pakka reads, writes, and edits every supported archive format with the
full operation set — list, extract, create, add, delete, verify. The
formats differ in their on-disk container, their filename width, and
whether they support compression; the CLI and library surface them
uniformly.

## Format selection at create time

`pakka -c <archive>` picks the output format from the destination
extension (case-insensitive):

| Extension                  | Format       |
| -------------------------- | ------------ |
| `.pk3`                     | PK3          |
| `.pk4`                     | PK4          |
| `.sin`                     | SiN          |
| `.wad`                     | PWAD (Doom)  |
| anything else              | PAK (Quake)  |

`--format <name>` overrides the extension. Names: `pak` (aliases
`goldsrc`, `hl`), `sin`, `daikatana`, `iwad`, `pwad`, `pk3`, `pk4`.
IWAD authoring is rare in practice, so `.wad` defaults to PWAD; pass
`--format iwad` explicitly when authoring a base IWAD.

At open time the on-disk magic and directory geometry are probed in
sequence (Quake PAK / SiN SPAK / Daikatana / Doom IWAD or PWAD / ZIP);
the format is then pinned for the rest of the session.
`pakka_open_ex` lets the library caller pin the format explicitly,
which matters for Daikatana since it shares Quake's `"PACK"` magic.

## Compression

PK3, PK4, and Daikatana support compression; Quake PAK and SiN do not.

**PK3 / PK4 (DEFLATE).** Defaults to uncompressed — ZIP method 0,
called `STORED` in the spec: the entry's bytes go into the archive
verbatim with no codec applied. Pass `--compress` to the CLI or call
`pakka_set_compression(archive, PAKKA_COMPRESSION_DEFLATE, &err)` from
the library to switch new adds to DEFLATE. Each entry's compressed
payload is computed up front; if DEFLATE wouldn't beat the raw bytes
for that entry — incompressible inputs like PNG, MP3, or random bytes
— the archive falls back to uncompressed for that entry automatically.
Matches info-zip behaviour.

**Daikatana (custom byte-codec).** Compressed entries use an LZSS-style
opcode stream (zero-run, byte-RLE, back-reference, literal) decoded by
`pakka_dk_inflate` and produced by `pakka_dk_deflate`. The encoder
follows the original Ion Storm packer's policy: entries whose name ends
in `.tga`, `.bmp`, `.wal`, `.pcx`, or `.bsp` are encoded; everything
else is stored uncompressed. Per-entry auto-fallback to uncompressed
when the encoder doesn't shrink the source.

The DEFLATE codec is selectable at build time. Default is the bundled
`sdefl` + `sinfl` single-header pair (no external dependency, ~1400 LOC
vendored under `src/vendor/`):

    make                                       # default: bundled codec
    cmake -B build/cmake .                     # same on Windows

Integrators who already link `zlib` can route DEFLATE through it
instead:

    make PAKKA_DEFLATE_BACKEND=zlib
    cmake -B build/cmake -DPAKKA_USE_ZLIB=ON .

Both backends produce byte-interchangeable raw RFC 1951 DEFLATE streams
— a PK3 written with one reads cleanly with the other, and with any
third-party tool (info-zip, Windows Explorer, the Quake III / Doom 3
engines).

## Daikatana specifics

Daikatana paks share Quake's `"PACK"` magic and 12-byte header but
widen the directory record from 64 to 72 bytes, adding a per-entry
`compressed_length` and `is_compressed` flag. Pakka probes both
directory layouts at open time; `--format daikatana` pins the decision
when the on-disk geometry is ambiguous.

The compression codec is documented opcode-by-opcode in
[`formats/daikatana-pak-format.md`](formats/daikatana-pak-format.md)
and in `src/dk_codec.c` in the repo. Reference decoder:
yquake2/pakextract.

## Doom IWAD / PWAD specifics

Doom 1 / 2 WAD ("Where's All the Data") is the structural ancestor of
the Quake PAK family — a 12-byte header followed by a flat directory
of fixed-size entries. Three things differ from PAK in ways that
matter:

* **Two magic variants.** `IWAD` is an id-shipped base archive; `PWAD`
  is a modder patch. Otherwise byte-identical; pakka treats them as
  label variants of one implementation (mirroring the PK3 / PK4
  split).
* **Lump names are 8 bytes**, ASCII, NUL-padded, no separator
  semantics. The hierarchy is encoded by marker lumps (`F_START`,
  `F_END`, etc.) rather than by `/` in names.
* **Duplicate lump names are legal.** Doom's loader picks the last
  occurrence; pakka preserves duplicates on round-trip.

`.wad` defaults to PWAD on create; pass `--format iwad` for the base
flavour. Full on-disk layout in
[`formats/wad-format.md`](formats/wad-format.md).

## PK3 / PK4 ZIP features

PK3 (Quake 3) and PK4 (Doom 3) are byte-identical ZIP containers; the
extension only changes the label returned by `pakka_format()`.

Pakka supports the subset of ZIP needed by the game engines:
uncompressed (`STORED`, method 0) and DEFLATE (method 8), single-disk
archives, no encryption, no streaming descriptors. The following are refused at
open or extract time with `PAKKA_ERR_UNSUPPORTED`:

* ZIP64 extensions (>4 GiB archives, >65k entries)
* encrypted entries (any traditional or strong encryption header)
* data descriptors (general-purpose bit 3)
* multi-disk archives
* compression methods other than 0 and 8

Each refusal returns a structured error with the offending feature
named in the message.

## On-disk format references

For format internals — directory geometry, header layout, byte
ordering, codec opcode tables — see the per-format references:

* [Quake PAK](formats/quake-pak-format.md) — Quake 1 /
  Quake 2 / GoldSrc (`"PACK"`, 56-byte names, 64-byte entries)
* [SiN](formats/sin-pak-format.md) — SiN (`"SPAK"`,
  120-byte names, 128-byte entries)
* [Daikatana](formats/daikatana-pak-format.md) —
  Daikatana (72-byte entries, custom byte-codec opcode table)
* [PK3 / PK4](formats/pk3-pk4-format.md) — PK3 / PK4 ZIP
  container subset
* [Doom WAD](formats/wad-format.md) — Doom IWAD / PWAD
  (12-byte header, 16-byte entries, 8-byte lump names, duplicates
  allowed)
* [Windows codepage](formats/windows-codepage.md) — PK3 / PK4
  filename encoding policy on Windows
