#!/usr/bin/env bats
#
# Daikatana pak format. Same "PACK" magic as Quake, but directory
# entries are 72 bytes (extra trailing u32 pair for compressed_size +
# is_compressed). Custom byte-codec compression with five opcode
# classes — see src/dk_codec.c. pakka supports DK writes: the encoder
# lives next to the decoder in src/dk_codec.c and the add path applies
# it automatically to .tga/.bmp/.wal/.pcx/.bsp entries (per the
# pakextract format notes).

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# Synthetic DK pak builder. Writes a PACK archive whose directory
# entries are 72 bytes. Each call appends one entry — caller threads
# the offsets manually for multi-entry paks. For single-entry paks the
# convenience helper write_dk_one_entry below covers the common case.
#
# All helpers use python3 because bash's printf is limited for binary
# emit; python3 is already a test dependency in the project workflow
# files.

u32() {
    python3 -c "import sys, struct; sys.stdout.buffer.write(struct.pack('<I', int(sys.argv[1])))" "$1"
}

pad_to() {
    local s="$1" n="$2"
    python3 -c "import sys; s=sys.argv[1].encode(); n=int(sys.argv[2]); sys.stdout.buffer.write(s + b'\0' * (n - len(s)))" "$s" "$n"
}

# Single-entry DK pak with a STORED (uncompressed) payload.
write_dk_one_stored() {
    local pakfile="$1" name="$2" payload="$3"
    local plen=${#payload}
    {
        printf 'PACK'
        u32 $((12 + plen))   # diroffset = past payload
        u32 72               # dirlength = one 72-byte entry
        printf '%s' "$payload"
        pad_to "$name" 56
        u32 12               # offset (immediately after header)
        u32 "$plen"          # length (uncompressed)
        u32 0                # compressed_size (unused for STORED)
        u32 0                # is_compressed = 0
    } > "$pakfile"
}

# Single-entry DK pak whose payload is encoded with one literal-run
# opcode (b = 0..63 means literal run of (b+1) bytes from input).
# The compressed stream is one opcode byte + the literal bytes + 0xFF
# terminator. payload_len is at most 64.
write_dk_one_literal_compressed() {
    local pakfile="$1" name="$2" payload="$3"
    local plen=${#payload}
    if [ "$plen" -lt 1 ] || [ "$plen" -gt 64 ]; then
        echo "write_dk_one_literal_compressed: payload must be 1..64 bytes" >&2
        return 1
    fi
    local clen=$((1 + plen + 1))   # opcode + bytes + 0xFF terminator
    {
        printf 'PACK'
        u32 $((12 + clen))
        u32 72
        # Compressed payload: opcode (plen-1) for "literal of plen bytes"
        # + the bytes + 0xFF terminator.
        python3 -c "import sys; sys.stdout.buffer.write(bytes([int(sys.argv[1])-1]))" "$plen"
        printf '%s' "$payload"
        printf '\377'
        pad_to "$name" 56
        u32 12
        u32 "$plen"          # length = uncompressed size
        u32 "$clen"          # compressed_size
        u32 1                # is_compressed = 1
    } > "$pak"
}

# A pak with dirlength = 576 — divisible by both 64 and 72. Used to
# exercise the PACK ambiguity probe. We craft entries that fail both
# layout probes by setting impossible offsets so the test sees the
# "ambiguous" or "does not parse" error rather than a false hit.
write_ambiguous_576() {
    local pakfile="$1"
    # Single 576-byte directory block of all-zero bytes (every entry
    # has offset=0 which fails the >= HEADER_SIZE check in both layouts).
    {
        printf 'PACK'
        u32 12
        u32 576
        python3 -c "import sys; sys.stdout.buffer.write(b'\\0' * 576)"
    } > "$pakfile"
}

# Inspect the directory of a DK pak and print one line per entry in the
# form "<name> <length> <compressed_size> <is_compressed>". Reads the
# 12-byte header to find diroffset / dirlength, then iterates 72-byte
# rows. Used to assert per-entry compression state without depending on
# pakka -l (which doesn't expose dk_is_compressed).
dk_probe_dir() {
    local pak="$1"
    python3 - "$pak" <<'PY'
import struct, sys
p = open(sys.argv[1], "rb").read()
assert p[0:4] == b"PACK", "bad magic"
diroffset, dirlength = struct.unpack_from("<II", p, 4)
assert dirlength % 72 == 0, "not a DK directory (dirlength % 72 != 0)"
for i in range(dirlength // 72):
    row = p[diroffset + i * 72 : diroffset + (i + 1) * 72]
    name = row[:56].split(b"\0", 1)[0].decode("latin-1")
    flen, csize, isc = struct.unpack_from("<III", row, 56 + 4)
    # (offset is at 56, we skip it for this report)
    print(f"{name} {flen} {csize} {isc}")
PY
}

# Print the physical end-of-payload byte for a DK pak (max offset +
# on-disk extent across all entries). Should equal diroffset on a
# well-formed write.
dk_probe_payload_end() {
    local pak="$1"
    python3 - "$pak" <<'PY'
import struct, sys
p = open(sys.argv[1], "rb").read()
assert p[0:4] == b"PACK"
diroffset, dirlength = struct.unpack_from("<II", p, 4)
assert dirlength % 72 == 0
end = 12
for i in range(dirlength // 72):
    row = p[diroffset + i * 72 : diroffset + (i + 1) * 72]
    off = struct.unpack_from("<I", row, 56)[0]
    flen, csize, isc = struct.unpack_from("<III", row, 56 + 4)
    extent = csize if isc else flen
    if off + extent > end:
        end = off + extent
print(end)
PY
}

@test "dk open: STORED-only archive lists" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "weapons/blast.mdl" "model-bytes-here"
    run "$PAKKA" -l "$pak" --format daikatana
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "weapons/blast.mdl"
}

@test "dk extract: STORED entry round-trips byte-identical" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "weapons/blast.mdl" "model-bytes-here"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out" "$pak"
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/weapons/blast.mdl")" = "model-bytes-here" ]
}

@test "dk extract: compressed (literal-run) entry decodes byte-identical" {
    pak="$BATS_TEST_TMPDIR/cmp.pak"
    write_dk_one_literal_compressed "$pak" "textures/wall.tga" "wall-pixels"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out" "$pak"
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/textures/wall.tga")" = "wall-pixels" ]
}

@test "dk create: empty archive then open-with-hint round-trips" {
    pak="$BATS_TEST_TMPDIR/empty.pak"
    run "$PAKKA" -c "$pak" --format daikatana </dev/null
    [ "$status" -eq 0 ]
    [ -f "$pak" ]
    # AUTO-open of empty PACK biases to Quake PAK; must pass --format
    # daikatana to verify identity.
    run "$PAKKA" -l "$pak" --format daikatana
    [ "$status" -eq 0 ]
}

@test "dk add: non-compressible extension stays STORED" {
    pak="$BATS_TEST_TMPDIR/add_stored.pak"
    src="$BATS_TEST_TMPDIR/test.txt"
    printf 'hello world' > "$src"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    run "$PAKKA" -a "$pak" --format daikatana --as test.txt "$src"
    [ "$status" -eq 0 ]
    # Directory probe: is_compressed must be 0 for non-listed extensions.
    out="$(dk_probe_dir "$pak")"
    echo "$out" | grep -qE '^test\.txt 11 0 0$'
    # Round-trip extract.
    mkdir -p "$BATS_TEST_TMPDIR/out_stored"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out_stored" "$pak"
    [ "$(cat "$BATS_TEST_TMPDIR/out_stored/test.txt")" = "hello world" ]
}

@test "dk add: compressible extension with redundant payload is encoded" {
    pak="$BATS_TEST_TMPDIR/add_compress.pak"
    src="$BATS_TEST_TMPDIR/zeros.bmp"
    # 4 KiB of zeros — encoder picks zero-run tokens for nearly all of it.
    python3 -c "import sys; sys.stdout.buffer.write(b'\\0' * 4096)" > "$src"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    run "$PAKKA" -a "$pak" --format daikatana --as zeros.bmp "$src"
    [ "$status" -eq 0 ]
    out="$(dk_probe_dir "$pak")"
    # length=4096, is_compressed=1, csize > 0 and < 4096
    csize="$(echo "$out" | awk '/^zeros\.bmp/ { print $3 }')"
    isc="$(echo "$out"   | awk '/^zeros\.bmp/ { print $4 }')"
    flen="$(echo "$out"  | awk '/^zeros\.bmp/ { print $2 }')"
    [ "$flen" = "4096" ]
    [ "$isc" = "1" ]
    [ "$csize" -gt 0 ]
    [ "$csize" -lt 4096 ]
    # Round-trip extract.
    mkdir -p "$BATS_TEST_TMPDIR/out_compress"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out_compress" "$pak"
    cmp "$src" "$BATS_TEST_TMPDIR/out_compress/zeros.bmp"
}

@test "dk add: compressible extension with incompressible payload falls back to STORED" {
    pak="$BATS_TEST_TMPDIR/add_fallback.pak"
    src="$BATS_TEST_TMPDIR/random.bmp"
    # Deterministic high-entropy bytes (LCG, fixed seed). Pseudo-random
    # output won't reach the min-3 thresholds for byte-RLE or back-ref,
    # so literal-only encoding (~256 + 5 = 261 bytes) overshoots the
    # 256-byte source and the STORED auto-fallback fires.
    python3 -c "
import sys
r = 0xC0FFEE
out = bytearray()
for _ in range(256):
    r = (r * 1103515245 + 12345) & 0xFFFFFFFF
    out.append((r >> 16) & 0xFF)
sys.stdout.buffer.write(bytes(out))" > "$src"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    run "$PAKKA" -a "$pak" --format daikatana --as random.bmp "$src"
    [ "$status" -eq 0 ]
    out="$(dk_probe_dir "$pak")"
    isc="$(echo "$out"   | awk '/^random\.bmp/ { print $4 }')"
    flen="$(echo "$out"  | awk '/^random\.bmp/ { print $2 }')"
    [ "$flen" = "256" ]
    [ "$isc" = "0" ]
    mkdir -p "$BATS_TEST_TMPDIR/out_fallback"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out_fallback" "$pak"
    cmp "$src" "$BATS_TEST_TMPDIR/out_fallback/random.bmp"
}

@test "dk add: mixed extensions, physical layout consistent" {
    pak="$BATS_TEST_TMPDIR/mixed.pak"
    txt="$BATS_TEST_TMPDIR/notes.txt"
    bmp="$BATS_TEST_TMPDIR/sky.bmp"
    printf 'plain text\n' > "$txt"
    python3 -c "import sys; sys.stdout.buffer.write(b'\\xAA' * 2048)" > "$bmp"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    "$PAKKA" -a "$pak" --format daikatana --as notes.txt "$txt"
    "$PAKKA" -a "$pak" --format daikatana --as sky.bmp "$bmp"
    out="$(dk_probe_dir "$pak")"
    # notes.txt -> STORED; sky.bmp -> compressed
    echo "$out" | grep -qE '^notes\.txt 11 0 0$'
    echo "$out" | grep -qE '^sky\.bmp 2048 [0-9]+ 1$'
    # diroffset must equal the highest payload-end across all entries.
    diroffset="$(python3 -c "
import struct, sys
p = open(sys.argv[1], 'rb').read()
print(struct.unpack_from('<I', p, 4)[0])" "$pak")"
    end="$(dk_probe_payload_end "$pak")"
    [ "$diroffset" = "$end" ]
}

@test "dk delete: surviving compressed entry stays bit-identical post-rebuild" {
    pak="$BATS_TEST_TMPDIR/delsurv.pak"
    a="$BATS_TEST_TMPDIR/keep.bmp"
    b="$BATS_TEST_TMPDIR/dropme.txt"
    # keep.bmp gets encoded; dropme.txt stays STORED. Delete dropme.txt
    # and verify keep.bmp survives the rebuild byte-identical.
    python3 -c "import sys; sys.stdout.buffer.write(b'\\x33' * 512 + b'\\x55' * 512)" > "$a"
    printf 'this entry will be deleted\n' > "$b"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    "$PAKKA" -a "$pak" --format daikatana --as keep.bmp "$a"
    "$PAKKA" -a "$pak" --format daikatana --as dropme.txt "$b"

    # Capture pre-delete bytes for keep.bmp by extracting once.
    mkdir -p "$BATS_TEST_TMPDIR/pre"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/pre" "$pak"
    cmp "$a" "$BATS_TEST_TMPDIR/pre/keep.bmp"

    # Delete dropme.txt (forces rebuild path in pakka_commit).
    "$PAKKA" -d "$pak" --format daikatana dropme.txt

    # Survivor must still decode byte-identical.
    mkdir -p "$BATS_TEST_TMPDIR/post"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/post" "$pak"
    cmp "$a" "$BATS_TEST_TMPDIR/post/keep.bmp"
    [ ! -e "$BATS_TEST_TMPDIR/post/dropme.txt" ]
    # Directory state: one entry left.
    out="$(dk_probe_dir "$pak")"
    [ "$(echo "$out" | wc -l)" -eq 1 ]
}

@test "dk verify: deep verify accepts well-formed compressed entry" {
    pak="$BATS_TEST_TMPDIR/cmp.pak"
    write_dk_one_literal_compressed "$pak" "textures/wall.tga" "good"
    run "$PAKKA" --verify --deep "$pak" --format daikatana
    [ "$status" -eq 0 ]
}

@test "dk verify: pakka-built archive passes --verify --deep" {
    pak="$BATS_TEST_TMPDIR/builtbypakka.pak"
    src="$BATS_TEST_TMPDIR/big.bmp"
    # 2 KiB of mixed content: 1 KiB of one byte, 1 KiB of another.
    python3 -c "import sys; sys.stdout.buffer.write(b'\\x11' * 1024 + b'\\x22' * 1024)" > "$src"
    "$PAKKA" -c "$pak" --format daikatana </dev/null
    "$PAKKA" -a "$pak" --format daikatana --as big.bmp "$src"
    run "$PAKKA" --verify --deep "$pak" --format daikatana
    [ "$status" -eq 0 ]
}

@test "dk verify: deep verify rejects compressed entry with file_length=0 and non-empty stream" {
    # Regression for a bug where pakka_verify's "length == 0" shortcut
    # fired before the format-specific extent resolution, letting a
    # DK entry with file_length=0, is_compressed=1, compressed_length>0
    # report OK without inspecting the opcode stream. Craft one with a
    # literal-of-5 opcode (which decodes to 5 bytes) declaring 0 bytes
    # of output — the decoder must reject.
    pak="$BATS_TEST_TMPDIR/dk_flen0.pak"
    {
        printf 'PACK'
        u32 $((12 + 6))      # diroffset
        u32 72
        # Payload: opcode 0x04 (literal of 5) + 5 bytes + terminator.
        # Out_len=0 means the literal-run-overruns-output guard fires.
        printf '\004'
        printf 'abcde'
        printf '\377'
        pad_to "cheat.tga" 56
        u32 12               # offset
        u32 0                # file_length = 0 (claims empty output)
        u32 6                # compressed_size
        u32 1                # is_compressed = 1
    } > "$pak"
    run "$PAKKA" --verify --deep "$pak" --format daikatana
    [ "$status" -ne 0 ]
}

@test "dk verify: deep verify rejects compressed entry with compressed_length=0 and non-empty declared length" {
    # Companion to the flen=0 regression: a DK compressed entry that
    # claims to decompress to 5 bytes but ships zero input bytes is
    # also corrupt. The codec given 0 input cannot produce 5 output;
    # the verify shortcut must not skip this case.
    pak="$BATS_TEST_TMPDIR/dk_csize0.pak"
    {
        printf 'PACK'
        u32 12               # diroffset = right after header (no payload)
        u32 72
        pad_to "cheat.tga" 56
        u32 12               # offset (no actual payload follows)
        u32 5                # file_length = 5 (claims 5 bytes of output)
        u32 0                # compressed_size = 0 (nothing to read)
        u32 1                # is_compressed = 1
    } > "$pak"
    run "$PAKKA" --verify --deep "$pak" --format daikatana
    [ "$status" -ne 0 ]
}

@test "dk read_entry_alloc: rejects malformed entry with file_length=0 + non-empty stream" {
    # pakka_read_entry_alloc used to shortcut on length==0 before
    # opening the entry. For a DK compressed entry with declared
    # uncompressed length 0 but a non-empty compressed payload, that
    # let a malformed archive return "success / empty data" without
    # invoking the codec. The CLI extract drives read_entry_alloc;
    # the malformed archive must surface as a non-zero exit.
    pak="$BATS_TEST_TMPDIR/dk_alloc_flen0.pak"
    {
        printf 'PACK'
        u32 $((12 + 6))
        u32 72
        printf '\004'
        printf 'abcde'
        printf '\377'
        pad_to "cheat.tga" 56
        u32 12
        u32 0
        u32 6
        u32 1
    } > "$pak"
    mkdir -p "$BATS_TEST_TMPDIR/out_alloc"
    run "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/out_alloc" "$pak"
    [ "$status" -ne 0 ]
}

@test "dk verify: deep verify rejects compressed entry with truncated stream" {
    # Craft a compressed payload that declares length=5 but the literal
    # run only carries 3 bytes before the 0xFF terminator. Deep verify
    # must reject.
    pak="$BATS_TEST_TMPDIR/bad.pak"
    {
        printf 'PACK'
        u32 $((12 + 5))      # diroffset
        u32 72
        # opcode 0x04 = literal run of 5 bytes... but we only supply 3
        # bytes before terminator. (Opcode 4 means b+1 = 5 literal bytes.)
        printf '\004'
        printf 'abc'
        printf '\377'
        pad_to "broken" 56
        u32 12
        u32 5                # length (uncompressed)
        u32 5                # compressed_size
        u32 1                # is_compressed
    } > "$pak"
    run "$PAKKA" --verify --deep "$pak" --format daikatana
    [ "$status" -ne 0 ]
}

@test "dk ambiguous PACK: bare 576-byte zero directory errors as ambiguous-or-bogus" {
    pak="$BATS_TEST_TMPDIR/amb.pak"
    write_ambiguous_576 "$pak"
    # The zero-filled directory has offset==0 entries — neither PAK nor
    # DK layout parses cleanly under the offset-validation probe, so we
    # expect a format error rather than a successful list.
    run "$PAKKA" -l "$pak"
    [ "$status" -ne 0 ]
}

@test "dk hint: PACK + --format daikatana skips probe and parses as DK" {
    # A 72-byte directory block. Parses correctly only as DK; Q2's
    # 64-byte interpretation would mis-read the trailing bytes.
    pak="$BATS_TEST_TMPDIR/dk_hint.pak"
    write_dk_one_stored "$pak" "test" "abc"
    run "$PAKKA" -l "$pak" --format daikatana
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "test"
}

@test "dk real fixture: external-packer pak opens, extracts, deep-verifies" {
    # Cross-validates pakka's decoder against a dkpak-built archive
    # (Daikatana's reference packer in -userpak mode). The staged
    # tree was maps/+textures/+root, so entry names carry those
    # prefixes while the source files in inputs/ are flat. Fixture
    # provenance + regen steps: tests/fixtures/dk/README.md.
    fixture="$BATS_TEST_DIRNAME/fixtures/dk/user.pak"
    inputs="$BATS_TEST_DIRNAME/fixtures/dk/inputs"
    # Hard fail (not skip) when the committed pak is gone, so CI
    # cannot lose this coverage to an accidental git-rm.
    if [ ! -f "$fixture" ]; then
        echo "FATAL: expected DK fixture missing at $fixture" >&2
        echo "       see tests/fixtures/dk/README.md for how to regenerate" >&2
        false
    fi

    # All four entries are listed under their routed names.
    run "$PAKKA" -l "$fixture" --format daikatana
    [ "$status" -eq 0 ]
    [[ "$output" == *"textures/zeros.bmp"*   ]]
    [[ "$output" == *"textures/striped.tga"* ]]
    [[ "$output" == *"maps/random.bsp"*      ]]
    [[ "$output" == *"notes.txt"*            ]]

    # Directory inspection: file_length and is_compressed flags.
    # Compressed-extension entries with redundant content must be
    # encoded; the random-bytes .bsp falls back to STORED via the
    # encoder's "encoded >= source" gate; the .txt entry is STORED
    # via the extension policy. csize is implementation-specific for
    # STORED entries (the format permits either 0 or = file_length),
    # so we don't pin it.
    out="$(dk_probe_dir "$fixture")"
    echo "$out" | grep -qE '^textures/zeros\.bmp 1024 [0-9]+ 1$'
    echo "$out" | grep -qE '^textures/striped\.tga 512 [0-9]+ 1$'
    echo "$out" | grep -qE '^maps/random\.bsp 256 [0-9]+ 0$'
    echo "$out" | grep -qE '^notes\.txt 33 [0-9]+ 0$'

    # Extract every entry and cmp byte-for-byte against the source
    # inputs. This is the cross-validation gate: pakka's decoder
    # consumes another implementation's encoded stream and must
    # reproduce the input exactly.
    mkdir -p "$BATS_TEST_TMPDIR/realout"
    "$PAKKA" -x --format daikatana -C "$BATS_TEST_TMPDIR/realout" "$fixture"
    cmp "$inputs/zeros.bmp"   "$BATS_TEST_TMPDIR/realout/textures/zeros.bmp"
    cmp "$inputs/striped.tga" "$BATS_TEST_TMPDIR/realout/textures/striped.tga"
    cmp "$inputs/random.bsp"  "$BATS_TEST_TMPDIR/realout/maps/random.bsp"
    cmp "$inputs/notes.txt"   "$BATS_TEST_TMPDIR/realout/notes.txt"

    # Deep verify drives the codec across every compressed entry and
    # confirms the on-disk extents are internally consistent.
    run "$PAKKA" --verify --deep "$fixture" --format daikatana
    [ "$status" -eq 0 ]
}
