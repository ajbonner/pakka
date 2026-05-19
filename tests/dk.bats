#!/usr/bin/env bats
#
# Daikatana pak format. Same "PACK" magic as Quake, but directory
# entries are 72 bytes (extra trailing u32 pair for compressed_size +
# is_compressed). Custom byte-codec compression with five opcode
# classes — see src/dk_codec.c. Pakka treats DK as read-only.

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
    } > "$pakfile"
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

@test "dk open: STORED-only archive lists" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "weapons/blast.mdl" "model-bytes-here"
    run "$PAKKA" -lf "$pak" --format daikatana
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "weapons/blast.mdl"
}

@test "dk extract: STORED entry round-trips byte-identical" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "weapons/blast.mdl" "model-bytes-here"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$pak" --format daikatana -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/weapons/blast.mdl")" = "model-bytes-here" ]
}

@test "dk extract: compressed (literal-run) entry decodes byte-identical" {
    pak="$BATS_TEST_TMPDIR/cmp.pak"
    write_dk_one_literal_compressed "$pak" "textures/wall.tga" "wall-pixels"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$pak" --format daikatana -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/textures/wall.tga")" = "wall-pixels" ]
}

@test "dk mutation: -a refused with --format daikatana" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "x" "y"
    src="$BATS_TEST_TMPDIR/src.txt"
    echo "z" > "$src"
    run "$PAKKA" -af "$pak" --format daikatana --as new "$src"
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "daikatana"
    echo "$output" | grep -qi "read-only"
}

@test "dk mutation: -d refused with --format daikatana" {
    pak="$BATS_TEST_TMPDIR/stored.pak"
    write_dk_one_stored "$pak" "weapons/blast.mdl" "y"
    run "$PAKKA" -df "$pak" --format daikatana weapons/blast.mdl
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "read-only"
}

@test "dk create: --format daikatana refused" {
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/should-not-exist.pak" --format daikatana </dev/null
    [ "$status" -ne 0 ]
    [ ! -f "$BATS_TEST_TMPDIR/should-not-exist.pak" ]
}

@test "dk verify: deep verify accepts well-formed compressed entry" {
    pak="$BATS_TEST_TMPDIR/cmp.pak"
    write_dk_one_literal_compressed "$pak" "textures/wall.tga" "good"
    run "$PAKKA" --verify --deep -f "$pak" --format daikatana
    [ "$status" -eq 0 ]
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
    run "$PAKKA" --verify --deep -f "$pak" --format daikatana
    [ "$status" -ne 0 ]
}

@test "dk ambiguous PACK: bare 576-byte zero directory errors as ambiguous-or-bogus" {
    pak="$BATS_TEST_TMPDIR/amb.pak"
    write_ambiguous_576 "$pak"
    # The zero-filled directory has offset==0 entries — neither PAK nor
    # DK layout parses cleanly under the offset-validation probe, so we
    # expect a format error rather than a successful list.
    run "$PAKKA" -lf "$pak"
    [ "$status" -ne 0 ]
}

@test "dk hint: PACK + --format daikatana skips probe and parses as DK" {
    # A 72-byte directory block. Parses correctly only as DK; Q2's
    # 64-byte interpretation would mis-read the trailing bytes.
    pak="$BATS_TEST_TMPDIR/dk_hint.pak"
    write_dk_one_stored "$pak" "test" "abc"
    run "$PAKKA" -lf "$pak" --format daikatana
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "test"
}
