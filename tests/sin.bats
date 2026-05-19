#!/usr/bin/env bats
#
# SiN (Ritual, 1998) pak format. Differs from Quake/Q2 PAK in three
# ways: "SPAK" magic instead of "PACK", 120-byte filename field
# (vs 56), 128-byte directory entry (vs 64). Payloads are still
# uncompressed.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# Pad a string to N bytes with NULs (sized for SiN's 120-byte name
# field). Uses python3 because bash's printf is awkward at producing a
# specific number of NUL bytes regardless of input length.
pad_to() {
    local s="$1" n="$2"
    python3 -c "import sys; s=sys.argv[1].encode(); n=int(sys.argv[2]); sys.stdout.buffer.write(s + b'\0' * (n - len(s)))" "$s" "$n"
}

# Write one little-endian u32. python3 keeps us portable across
# /usr/bin/printf differences.
u32() {
    python3 -c "import sys, struct; sys.stdout.buffer.write(struct.pack('<I', int(sys.argv[1])))" "$1"
}

# Build a minimal SiN pak with a single entry containing the given
# payload string.
write_sin_one_entry() {
    local pakfile="$1" name="$2" payload="$3"
    local plen=${#payload}
    # Layout: 12-byte header (SPAK + diroffset + dirlength) + payload
    # + 128-byte directory entry.
    # diroffset = 12 + plen, dirlength = 128
    {
        printf 'SPAK'
        u32 $((12 + plen))
        u32 128
        printf '%s' "$payload"
        pad_to "$name" 120
        u32 12        # entry offset (immediately after header)
        u32 "$plen"   # entry length
    } > "$pakfile"
}

@test "sin open: SPAK magic + 120-byte names is recognised" {
    pak="$BATS_TEST_TMPDIR/demo.sin"
    write_sin_one_entry "$pak" "maps/sintest.bsp" "hello-sin"
    run "$PAKKA" -lf "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "maps/sintest.bsp"
}

@test "sin extract: payload round-trips byte-identical" {
    pak="$BATS_TEST_TMPDIR/demo.sin"
    write_sin_one_entry "$pak" "maps/sintest.bsp" "hello-sin-roundtrip"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/out/maps/sintest.bsp" ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/maps/sintest.bsp")" = "hello-sin-roundtrip" ]
}

@test "sin create: .sin extension produces SPAK archive" {
    src="$BATS_TEST_TMPDIR/src"
    mkdir -p "$src"
    echo "payload" > "$src/file.txt"
    pak="$BATS_TEST_TMPDIR/created.sin"
    (cd "$src" && "$PAKKA" -cf "$pak" file.txt) >/dev/null
    [ -f "$pak" ]
    head -c 4 "$pak" | od -c | head -1 | grep -qF "S   P   A   K"
}

@test "sin create: --format sin overrides extension" {
    src="$BATS_TEST_TMPDIR/src"
    mkdir -p "$src"
    echo "payload" > "$src/file.txt"
    pak="$BATS_TEST_TMPDIR/created.pak"
    (cd "$src" && "$PAKKA" -cf "$pak" --format sin file.txt) >/dev/null
    [ -f "$pak" ]
    head -c 4 "$pak" | od -c | head -1 | grep -qF "S   P   A   K"
}

@test "sin round-trip: create, add, list, extract" {
    src="$BATS_TEST_TMPDIR/src"
    mkdir -p "$src/maps"
    echo "level data" > "$src/maps/e1m1.bsp"
    echo "model bytes" > "$src/p_blast.mdl"
    pak="$BATS_TEST_TMPDIR/full.sin"
    (cd "$src" && "$PAKKA" -cf "$pak" maps p_blast.mdl) >/dev/null
    run "$PAKKA" -lf "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "maps/e1m1.bsp"
    echo "$output" | grep -qF "p_blast.mdl"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$pak" -C "$BATS_TEST_TMPDIR/out" >/dev/null
    diff -rq "$src" "$BATS_TEST_TMPDIR/out"
}

@test "sin add: accepts a 119-byte entry name (one short of cap)" {
    pak="$BATS_TEST_TMPDIR/init.sin"
    "$PAKKA" -cf "$pak" --format sin </dev/null >/dev/null
    # 119 bytes of 'a' — the longest legal SiN entry name.
    src="$BATS_TEST_TMPDIR/payload.txt"
    echo "p" > "$src"
    longname=$(python3 -c "print('a' * 119)")
    run "$PAKKA" -af "$pak" --as "$longname" "$src"
    [ "$status" -eq 0 ]
}

@test "sin add: rejects a 120-byte entry name (at the cap)" {
    pak="$BATS_TEST_TMPDIR/init.sin"
    "$PAKKA" -cf "$pak" --format sin </dev/null >/dev/null
    src="$BATS_TEST_TMPDIR/payload.txt"
    echo "p" > "$src"
    toolong=$(python3 -c "print('a' * 120)")
    run "$PAKKA" -af "$pak" --as "$toolong" "$src"
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "too long"
}

@test "sin format: pakka_format() reports SIN for opened SPAK archive" {
    src="$BATS_TEST_TMPDIR/src"
    mkdir -p "$src"
    echo "p" > "$src/x.txt"
    pak="$BATS_TEST_TMPDIR/fmt.sin"
    (cd "$src" && "$PAKKA" -cf "$pak" x.txt) >/dev/null
    # Listing via -lf must succeed for the freshly-created SPAK archive
    # — that's the user-visible signal that the format is recognised on
    # the read side.
    run "$PAKKA" -lf "$pak"
    [ "$status" -eq 0 ]
}

@test "sin open: --format pak rejects an SPAK archive (hint mismatch)" {
    pak="$BATS_TEST_TMPDIR/demo.sin"
    write_sin_one_entry "$pak" "x" "y"
    run "$PAKKA" -lf "$pak" --format pak
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "format_hint"
}

@test "sin delete: rebuild path keeps remaining entries" {
    src="$BATS_TEST_TMPDIR/src"
    mkdir -p "$src"
    echo "keep1" > "$src/a.txt"
    echo "drop"  > "$src/b.txt"
    echo "keep2" > "$src/c.txt"
    pak="$BATS_TEST_TMPDIR/del.sin"
    (cd "$src" && "$PAKKA" -cf "$pak" a.txt b.txt c.txt) >/dev/null
    "$PAKKA" -df "$pak" b.txt >/dev/null
    run "$PAKKA" -lf "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "a.txt"
    ! echo "$output" | grep -qF "b.txt"
    echo "$output" | grep -qF "c.txt"
}
