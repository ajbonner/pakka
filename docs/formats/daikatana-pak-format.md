# Daikatana PAK format

A complete reference for the Daikatana (Ion Storm, 2000) PAK archive
format as implemented by pakka.

Daikatana paks share Quake's `PACK` magic and 12-byte header but widen
the directory record from 64 to 72 bytes, adding a per-entry
`compressed_length` and `is_compressed` flag. Compressed entries use a
custom byte-codec (LZSS-style) documented in §3 below.

## 1. Sources

References that informed this document:

- Daniel Gibson, "Daikatana .pak format" (gist) —
  <https://gist.github.com/DanielGibson/8bde6241c93e5efe8b75e5e00d0b9858>.
  This is the canonical narrative reference and the basis for §2 and
  the decoder opcode table in §3.
- yquake2/pakextract — <https://github.com/yquake2/pakextract>
  (BSD-2-Clause). Reference C decoder; pakka's `pakka_dk_inflate` in
  `src/dk_codec.c` matches its behaviour exactly.
- `dktools_readme.htm` (John Romero, Ion Storm 2000) — public
  documentation shipped with the Daikatana editing-tools distribution
  (`dktools_11h`). Section 4 covers `dkpak.exe`, the packer Ion Storm
  used to build the game's shipped paks. Default mode auto-allocates
  files by type into three output paks (`base` / `models` / `maps`);
  `-userpak` mode collapses that into a single pak and preserves
  whatever staging directory tree the author provides.
- pakka's own implementation: `src/dk_codec.c` (decoder + encoder),
  `src/pakfile.c` (directory I/O, add-path policy, on-disk-extent
  helper), and `src/common.c` (PAK-class geometry table).

The encoder strategy in §4 is pakka's own design, justified by per-token
byte-cost analysis. The encoder produces streams that the decoder in
§3 accepts; correctness is checked at every layer by round-tripping
encoder output through the decoder in `test/dk_codec_test.c`, and
cross-validated against `dkpak`-produced output in
`test/fixtures/dk/`.

## 2. File layout

### 2.1 Header — 12 bytes

| Offset | Size | Field       | Notes                                          |
| ------ | ---- | ----------- | ---------------------------------------------- |
| 0      | 4    | `signature` | `"PACK"` — bit-identical to Quake / Q2 PAK.    |
| 4      | 4    | `diroffset` | u32 LE — byte offset of the directory block.   |
| 8      | 4    | `dirlength` | u32 LE — directory size in bytes (72 × N).     |

Because the magic collides with Quake PAK, an open-time layout probe
disambiguates: each candidate geometry (64-byte Quake row vs 72-byte
Daikatana row) is validated against every directory entry's
offset + on-disk extent vs the file size. If exactly one parses
cleanly, it wins; if both parse, callers must pass `--format
daikatana` (or `PAKKA_FORMAT_DAIKATANA` to `pakka_open_ex`) to
disambiguate. Empty archives (`dirlength == 0`) bias to Quake PAK
unless the DK hint is explicit. See `probe_pak_layout` in
`src/pakfile.c`.

### 2.2 Directory entry — 72 bytes

| Offset | Size | Field               | Notes                                                                |
| ------ | ---- | ------------------- | -------------------------------------------------------------------- |
| 0      | 56   | `filename`          | NUL-padded; forward-slash path separators.                           |
| 56     | 4    | `file_pos`          | u32 LE — offset of payload bytes in the archive.                     |
| 60     | 4    | `file_length`       | u32 LE — **uncompressed** size.                                      |
| 64     | 4    | `compressed_length` | u32 LE — on-disk byte extent for compressed entries; 0 for STORED.   |
| 68     | 4    | `is_compressed`     | u32 LE — `0` = STORED (raw bytes), non-zero = encoded per §3.        |

**Mixed STORED + compressed in the same archive is legal.** Compression
is a per-entry decision; the same archive can carry both. pakka's
encoder uses this to apply compression selectively (§5).

**`file_length` is always the uncompressed size.** For STORED entries
this is also the on-disk byte extent; for compressed entries the
on-disk extent is `compressed_length`. Every site in pakka that walks
the byte layout — `copy_between_paks`, `compute_payload_end`,
`write_pak_directory` — routes through the
`pak_entry_on_disk_extent(e)` helper to pick the right value.

### 2.3 Endianness and limits

Every on-disk u32 is little-endian regardless of host byte order. In
pakka the directory I/O sites use `pakka_read_u32_le` /
`pakka_write_u32_le` from `src/common.c`; the CI matrix includes a
big-endian s390x job to keep this honest. Offsets and lengths are
capped at `UINT32_MAX` (4 GiB) at every write site.

## 3. Wire format: byte-codec opcode table

Compressed entries (`is_compressed != 0`) carry a stream of variable-
length opcodes. The decoder reads one control byte `c` at a time and
dispatches:

| Control byte | Class       | Length expression | Payload                    | Output                                                |
| ------------ | ----------- | ----------------- | -------------------------- | ----------------------------------------------------- |
| `0x00..0x3F` | literal run | `c + 1` (1..64)   | `c+1` raw bytes from input | Copy them straight to output.                         |
| `0x40..0x7F` | zero run    | `c - 0x3E` (2..65)| (none)                     | Write `c - 0x3E` zero bytes.                          |
| `0x80..0xBF` | byte run    | `c - 0x7E` (2..65)| 1 byte                     | Write the payload byte `c - 0x7E` times.              |
| `0xC0..0xFD` | back-ref    | `c - 0xBE` (2..63)| 1 byte (`off`)             | Copy `c - 0xBE` bytes from `produced - (off + 2)`.    |
| `0xFE`       | invalid     | —                 | —                          | Format error.                                         |
| `0xFF`       | terminator  | —                 | (none)                     | End of stream.                                        |

Back-reference distance is `off + 2`, range `2..257`. The copy is into
*already-decoded output*, so LZ-style overlap is legal: when the copy
length exceeds the distance, the freshly-written tail bytes feed back
into the same copy operation, repeating a window of `distance` bytes.
The decoder must do this byte-by-byte (not `memcpy` / `memmove`) for
correctness.

**Termination.** A well-formed stream ends with `0xFF`, but the decoder
also accepts clean input exhaustion provided `produced == out_len`. Any
under-fill (`produced < out_len` at end-of-input) is a format error.

**Strict bounds.** Every input read is checked against `in_len`, every
output write against `out_len`, every back-reference distance against
`produced`. Any violation returns `PAKKA_ERR_FORMAT`; the output tail
beyond the failure point is left uninitialised and callers must treat
partial output as garbage.

### 3.1 Implementation in pakka

`pakka_dk_inflate(const unsigned char *in, size_t in_len,
                  unsigned char *out, size_t out_len, pakka_error_t *err)`
in `src/dk_codec.c`. Whole-buffer decode — pakka allocates a buffer
of size `entry->file_length` and runs the codec to completion. The
per-archive `max_decompressed` cap (default 64 MiB, settable via
`pakka_set_max_decompressed_size`) bounds peak RSS against a malicious
or accidentally-oversized declared `file_length`.

## 4. Encoder strategy

pakka's `pakka_dk_deflate` is a single-pass greedy matcher over the
back-reference distance range (2..257). At each input position it
gathers four candidate encodings, picks the best by token cost, and
emits.

### 4.1 Candidate tokens

For each input position `i`:

- **`z` — zero-run length.** Largest `n` such that `in[i .. i+n-1]`
  are all `0x00`, capped at `DK_MAX_ZERO_RUN = 64`.
- **`r` — byte-RLE length.** Largest `n` such that `in[i .. i+n-1]`
  are all `in[i]`, capped at `DK_MAX_BYTE_RUN = 64`.
- **`m` / `dist` — best back-reference.** Searches distances `d` in
  `[2, 257]` for the longest match starting at `i`, with match length
  capped at `min(DK_MAX_BACK_REF, d, remaining)` where
  `DK_MAX_BACK_REF = 62`. The fast-path filter `in[i - d] != in[i]`
  skips most candidates without the inner loop.
- **literal byte** — always available as a fallback (cost 1 byte
  amortised inside a 64-byte literal run).

### 4.2 Conservative caps

The encoder uses caps **one step inside** the decoder envelope:

| Token        | Decoder accepts | Encoder emits |
| ------------ | --------------- | ------------- |
| Zero run     | 2..65           | 2..64         |
| Byte-RLE     | 2..65           | 2..64         |
| Back-ref     | 2..63           | 3..62         |
| Literal      | 1..64           | 1..64         |

A length-2 byte-RLE or back-ref is a 2-byte opcode replacing 2
literal bytes — break-even on bytes, but it splits any in-progress
literal run and forces a re-open downstream. Skipping these tokens
avoids encodings that almost always cost more than they save.

### 4.3 No-overlap rule

The encoder caps each back-reference at length ≤ distance. The
decoder accepts longer back-refs via LZ-style overlap (§3), but the
encoder skips that case. Cost in compression ratio is small (overlap
matches only win for highly repetitive patterns like `"ababab..."`) and
the benefit is a single-pass matcher with no overlap special case.

### 4.4 Decision tree

Once `z`, `r`, `m` are gathered, sub-threshold candidates are zeroed
out (the minimum profitable lengths are zero-run 2, byte-RLE and
back-ref 3, capturing the per-token byte cost). The remaining choice:

```
if (z == 0 && r == 0 && m == 0) {
    emit literal byte (deferred — accumulated in a 64-byte run)
} else if (z * 2 > r && z * 2 > m) {
    emit zero-run of length z
} else if (m > r) {
    emit back-ref of length m at distance dist
} else {
    emit byte-RLE of length r
}
```

**Zero-run preference (the `2 ×` tiebreak).** A zero-run is a 1-byte
opcode (no payload). Byte-RLE and back-ref are 2-byte opcodes (op +
payload). At equal length the zero-run saves one byte vs the
alternatives, so it deserves preference; doubling the score reflects
that asymmetry across a range of competing lengths.

**Greedy.** No deferred matching or lazy evaluation. Whichever encoding
wins at position `i` is committed immediately, and `i` advances by the
match length. Greedy is enough for typical Daikatana content
(palettized images, BSP files); the round-trip test suite catches any
encoder bug that would produce decoder-invalid output.

### 4.5 Worst-case output bound

Worst case is literal-only: every input byte costs 1 byte plus
`ceil(in_len / 64)` opcode bytes plus 1 terminator. In u64:

```
worst = in_len + (in_len + 63) / 64 + 1
```

For `in_len = 4 GiB - 1` this slightly exceeds `UINT32_MAX`. Callers
(see `pakka_add_file` and `pakka_add_memory` in `src/pakfile.c`)
compute this bound in `uint64_t`, compare against `UINT32_MAX`, and
skip the encoder entirely if it would overflow — falling back to
STORED for the source. The on-disk `compressed_length` field is u32,
so anything that wouldn't fit must be STORED.

### 4.6 STORED auto-fallback

After encoding, the caller compares `encoded_size` against
`source_size`. If the encoded form is **not strictly smaller** than the
source, the entry is rewritten as STORED instead — same policy as
info-zip and as pakka's PK3 / PK4 DEFLATE add path. The directory's
`is_compressed` flag is cleared and `compressed_length` left at 0.

## 5. Add-path extension policy

pakka applies the encoder automatically based on the entry name's
extension. The five extensions Daniel Gibson's gist documents as
"compressed when packing" are the policy in `src/pakfile.c`'s
`dk_compressible_extension`:

| Extension | Typical Daikatana content                                |
| --------- | -------------------------------------------------------- |
| `.tga`    | Targa textures.                                          |
| `.bmp`    | Windows bitmap textures.                                 |
| `.wal`    | Quake 2-style wall textures (palettized).                |
| `.pcx`    | PCX images.                                              |
| `.bsp`    | Compiled map files.                                      |

Match is case-insensitive, ASCII-only (no `tolower()` — locale-
dependent). Everything else is STORED. There is no user knob;
`--compress` stays a PK3 / PK4 affordance. DK callers select between
STORED and compressed by naming entries appropriately.

## 6. I/O integration

### 6.1 Reading

`pakka_open_ex(path, PAKKA_OPEN_READ, PAKKA_FORMAT_DAIKATANA, ...)` skips
the PACK ambiguity probe and asserts the on-disk magic is `PACK`. The
directory loader parses 72-byte rows including the two extra u32s; on
mixed-archive content the per-entry `is_compressed` flag drives whether
`pakka_open_entry` / `pakka_reader_read` route through the codec or read
raw bytes.

### 6.2 Writing

`pakka_create(path, PAKKA_FORMAT_DAIKATANA, ...)` opens a temp file and
stamps a 12-byte `PACK` header with `dirlength = 0`. Subsequent
`pakka_add_file` / `pakka_add_memory` calls run the encoder per the
extension policy (§5) and update the in-memory entry list with
`dk_is_compressed` / `dk_compressed_size`. Commit modes:

- **Add-only.** `pakka_commit` (also called implicitly by
  `pakka_close`) writes the directory at byte-offset
  `tail->offset + pak_entry_on_disk_extent(tail)` and rewrites the
  header.
- **Rebuild.** Any `pakka_delete` forces a rebuild via a scratch temp:
  surviving entries get copied (their on-disk extents, opaque to the
  rebuild — compressed entries pass through unchanged), a fresh
  directory is written, and the scratch is renamed over the original.

The 4 GiB append-limit check uses the **physical** write size (encoded
or STORED, whichever was committed), not `entry->length`. Set in
`pakka_add_file` / `pakka_add_memory` via `physical_size`.

### 6.3 Verify

`pakka_verify` walks every entry, runs the name-safety check, streams
the payload to confirm `offset` and on-disk extent point at readable
bytes, and flags portable-union collisions on extraction. With
`PAKKA_VERIFY_DEEP`, compressed entries are inflated through
`pakka_dk_inflate` to confirm the byte-codec stream is well-formed and
produces exactly `file_length` output bytes (the same exact-length
contract the on-disk format requires).

## 7. Cross-format notes

DK shares the `PACK` magic with Quake / Q2 PAK and the 56-byte
filename field. The differences are entirely in the directory layout
(72 vs 64 bytes) and the compression flag. pakka's PAK-class geometry
table in `src/common.c` enumerates the three rows side by side:

```
                   signature  name_field_len  dir_entry_size  has_compression
Quake / Q2 PAK      "PACK"            56               64             no
SiN                 "SPAK"           120              128             no
Daikatana           "PACK"            56               72            yes
```

Every PAK-class read/write site dispatches off the row, so adding a
new variant means appending a row, not forking every code path.

## 8. Test coverage

- `test/dk_codec_test.c` — direct codec exerciser. Decoder covers
  every opcode class, bounds violations, termination contracts.
  Encoder covers empty input, single literal, zero run, byte-RLE,
  alternating pattern (back-ref), high-entropy fallback, undersized
  output buffer, and a 4 KiB mixed-payload round-trip.
- `test/dk_test.c` — end-to-end CLI tests. STORED open / extract,
  compressed open / extract, create empty, add STORED (non-listed
  ext), add compressed (listed ext + redundant payload), add STORED
  fallback (listed ext + incompressible payload), mixed-extension
  layout check, delete-with-compressed-survivors round-trip, deep
  verify on both synthetic and pakka-built archives, ambiguity
  detection.
- `test/c_api_test.c` — public API round-trip: `pakka_create` for
  DK + `pakka_add_memory` of compressible bytes + `pakka_close` +
  `pakka_open_ex` with DK hint + `pakka_read_entry_alloc` + memcmp.

Cross-validation against an external encoder is wired up at
`test/fixtures/dk/`: synthetic input bytes (`inputs/*`, generated by
`inputs/generate.py`) are packed into `user.pak` by `dkpak`;
`test/dk_test.c` consumes the result and byte-compares every
extracted payload against the committed source. The case hard-fails
when `user.pak` is missing so accidental deletion can't silently
drop the coverage.
