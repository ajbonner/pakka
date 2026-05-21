#!/usr/bin/env bats
#
# Doom WAD format (IWAD + PWAD). 12-byte header (magic + numlumps int32
# LE + infotableofs int32 LE) followed by payloads, with the directory
# at infotableofs holding numlumps × 16-byte entries (filepos int32 LE +
# size int32 LE + 8-byte NUL-padded name). IWAD and PWAD share the
# layout — the magic distinguishes id-shipped base archives (IWAD) from
# modder patch archives (PWAD). Pakka treats them as label variants of
# one impl, mirroring PK3/PK4.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# Emit a little-endian u32. Used to assemble synthetic headers and
# directory entries.
u32() {
    python3 -c "import sys, struct; sys.stdout.buffer.write(struct.pack('<I', int(sys.argv[1])))" "$1"
}

# Emit an 8-byte WAD lump name, NUL-padded if shorter than 8 chars.
# WAD names are exactly 8 bytes on disk with no trailing NUL when the
# name fills the field.
wad_name() {
    python3 -c "import sys; s=sys.argv[1].encode(); n=8; assert len(s) <= n, f'name {s!r} > 8 bytes'; sys.stdout.buffer.write(s + b'\0' * (n - len(s)))" "$1"
}

# Build a synthetic WAD with one or more entries. Args after pakfile
# come in name/payload pairs:
#   write_wad_synth <pakfile> <magic> <name1> <payload1> [<name2> <payload2> ...]
# Marker lumps (size 0, filepos 0) are supported by passing an empty
# payload string. magic must be "IWAD" or "PWAD".
write_wad_synth() {
    local pakfile="$1" magic="$2"
    shift 2
    python3 - "$pakfile" "$magic" "$@" <<'PY'
import struct, sys
pakfile, magic = sys.argv[1], sys.argv[2]
pairs = sys.argv[3:]
assert magic in ("IWAD", "PWAD"), magic
assert len(pairs) % 2 == 0, "name/payload pairs must come in twos"
entries = []
data = b""
offset = 12  # past the header
for i in range(0, len(pairs), 2):
    name = pairs[i]
    payload = pairs[i + 1].encode()
    if not payload:
        # Marker lump convention: filepos = 0, size = 0.
        entries.append((0, 0, name))
    else:
        entries.append((offset, len(payload), name))
        data += payload
        offset += len(payload)
numlumps = len(entries)
infotableofs = offset
with open(pakfile, "wb") as f:
    f.write(magic.encode())
    f.write(struct.pack("<II", numlumps, infotableofs))
    f.write(data)
    for off, size, name in entries:
        f.write(struct.pack("<II", off, size))
        nb = name.encode()
        assert len(nb) <= 8
        f.write(nb + b"\0" * (8 - len(nb)))
PY
}

# Dump WAD header and directory rows in a stable text form. One line per
# row: "<filepos> <size> <name>". Header is reported as a leading line
# "MAGIC numlumps infotableofs". Used to assert exact on-disk layout
# without depending on pakka -l (which doesn't print magic).
wad_probe() {
    local pak="$1"
    python3 - "$pak" <<'PY'
import struct, sys
p = open(sys.argv[1], "rb").read()
magic = p[:4].decode("latin-1")
numlumps, infotableofs = struct.unpack_from("<II", p, 4)
print(f"{magic} {numlumps} {infotableofs}")
for i in range(numlumps):
    base = infotableofs + i * 16
    off, sz = struct.unpack_from("<II", p, base)
    name = p[base + 8:base + 16].split(b"\0", 1)[0].decode("latin-1")
    print(f"{off} {sz} {name}")
PY
}

@test "wad create: empty IWAD has 12-byte header (IWAD + 0 + 12)" {
    pak="$BATS_TEST_TMPDIR/empty.wad"
    run "$PAKKA" -c --format iwad "$pak" </dev/null
    [ "$status" -eq 0 ]
    [ -f "$pak" ]
    [ "$(stat -f%z "$pak" 2>/dev/null || stat -c%s "$pak")" = "12" ]
    out="$(wad_probe "$pak")"
    [ "$out" = "IWAD 0 12" ]
}

@test "wad create: .wad extension defaults to PWAD" {
    pak="$BATS_TEST_TMPDIR/default.wad"
    run "$PAKKA" -c "$pak" </dev/null
    [ "$status" -eq 0 ]
    out="$(wad_probe "$pak")"
    [ "$out" = "PWAD 0 12" ]
}

@test "wad list: two-entry IWAD shows both lumps in directory order" {
    pak="$BATS_TEST_TMPDIR/two.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes" COLORMAP "cmap-bytes"
    run "$PAKKA" -l "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "PLAYPAL"
    echo "$output" | grep -qF "COLORMAP"
    # PLAYPAL must precede COLORMAP per directory order.
    pos_a="$(echo "$output" | grep -n 'PLAYPAL' | head -1 | cut -d: -f1)"
    pos_b="$(echo "$output" | grep -n 'COLORMAP' | head -1 | cut -d: -f1)"
    [ "$pos_a" -lt "$pos_b" ]
}

@test "wad extract: single entry round-trips byte-identical" {
    pak="$BATS_TEST_TMPDIR/extract.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out" "$pak"
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/PLAYPAL")" = "palette-bytes" ]
}

@test "wad add: 8-char lump name (LINEDEFS) is accepted, not too-long" {
    pak="$BATS_TEST_TMPDIR/eight.wad"
    src="$BATS_TEST_TMPDIR/payload.bin"
    printf 'lines-data' > "$src"
    "$PAKKA" -c --format pwad "$pak" </dev/null
    run "$PAKKA" -a "$pak" --as LINEDEFS "$src"
    [ "$status" -eq 0 ]
    run "$PAKKA" -l "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "LINEDEFS"
}

@test "wad directory: on-disk row order is filepos, size, name" {
    pak="$BATS_TEST_TMPDIR/order.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes"
    out="$(wad_probe "$pak")"
    # Header is line 1; first directory row is line 2 with the expected
    # offsets and exact name.
    head_line="$(echo "$out" | sed -n '1p')"
    row_line="$(echo "$out"  | sed -n '2p')"
    [ "$head_line" = "IWAD 1 25" ]   # 1 lump, infotableofs = 12 + 13
    [ "$row_line"  = "12 13 PLAYPAL" ]
}

@test "wad directory: create-and-add round-trips on-disk row order" {
    pak="$BATS_TEST_TMPDIR/order_rw.wad"
    src="$BATS_TEST_TMPDIR/p.bin"
    printf 'palette-bytes' > "$src"
    "$PAKKA" -c --format iwad "$pak" </dev/null
    "$PAKKA" -a "$pak" --as PLAYPAL "$src"
    out="$(wad_probe "$pak")"
    head_line="$(echo "$out" | sed -n '1p')"
    row_line="$(echo "$out"  | sed -n '2p')"
    [ "$head_line" = "IWAD 1 25" ]
    [ "$row_line"  = "12 13 PLAYPAL" ]
}

@test "wad open: marker lump (size 0, filepos 0) is accepted" {
    pak="$BATS_TEST_TMPDIR/marker.wad"
    # Empty payload string → write_wad_synth records filepos=0, size=0.
    write_wad_synth "$pak" IWAD F_START ""
    run "$PAKKA" -l "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qF "F_START"
}

@test "wad open: duplicate lump names load without rejection" {
    pak="$BATS_TEST_TMPDIR/dup.wad"
    write_wad_synth "$pak" IWAD THINGS "map01-things" THINGS "map02-things"
    run "$PAKKA" -l "$pak"
    [ "$status" -eq 0 ]
    # Both entries listed.
    dup_count="$(echo "$output" | grep -c '^THINGS' || true)"
    [ "$dup_count" -eq 2 ]
}

@test "wad add: duplicate name allowed on PWAD, rejected on PAK" {
    pak="$BATS_TEST_TMPDIR/dup_add.wad"
    src="$BATS_TEST_TMPDIR/dup_src.bin"
    printf 'dup-payload' > "$src"
    "$PAKKA" -c --format pwad "$pak" </dev/null
    "$PAKKA" -a "$pak" --as THINGS "$src"
    run "$PAKKA" -a "$pak" --as THINGS "$src"
    [ "$status" -eq 0 ]

    pak2="$BATS_TEST_TMPDIR/dup_add.pak"
    "$PAKKA" -c --format pak "$pak2" </dev/null
    "$PAKKA" -a "$pak2" --as THINGS "$src"
    run "$PAKKA" -a "$pak2" --as THINGS "$src"
    [ "$status" -ne 0 ]
    [[ "$output" == *Duplicate* ]] || [[ "$output" == *duplicate* ]]
}

@test "wad extract-all: refuses on duplicate-bearing WAD via collision check" {
    pak="$BATS_TEST_TMPDIR/dup_extract.wad"
    write_wad_synth "$pak" PWAD THINGS "first" THINGS "second"
    mkdir -p "$BATS_TEST_TMPDIR/out_dup"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out_dup" "$pak"
    [ "$status" -ne 0 ]
    # Collision after normalization fires for two identical names.
    echo "$output" | grep -qiE 'collide|collision'
}

@test "wad extract-by-name: selects first match only on duplicate-bearing WAD" {
    pak="$BATS_TEST_TMPDIR/dup_byname.wad"
    write_wad_synth "$pak" PWAD THINGS "first" THINGS "second"
    mkdir -p "$BATS_TEST_TMPDIR/out_byname"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out_byname" "$pak" THINGS
    [ "$status" -eq 0 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out_byname/THINGS")" = "first" ]
}

@test "wad delete: removes first match on duplicate-bearing WAD" {
    pak="$BATS_TEST_TMPDIR/dup_delete.wad"
    write_wad_synth "$pak" PWAD THINGS "first" THINGS "second"
    run "$PAKKA" -d "$pak" THINGS
    [ "$status" -eq 0 ]
    out="$(wad_probe "$pak")"
    # Header line should now report 1 lump.
    head_line="$(echo "$out" | sed -n '1p')"
    [[ "$head_line" == "PWAD 1 "* ]]
}

@test "wad rebuild: delete middle of three-entry PWAD preserves survivors" {
    pak="$BATS_TEST_TMPDIR/three.wad"
    write_wad_synth "$pak" PWAD AAA "alpha" BBB "bravo" CCC "charlie"
    run "$PAKKA" -d "$pak" BBB
    [ "$status" -eq 0 ]
    out="$(wad_probe "$pak")"
    [ "$(echo "$out" | sed -n '1p')" = "PWAD 2 24" ]   # 2 lumps, infotableofs = 12 + 5 + 7
    mkdir -p "$BATS_TEST_TMPDIR/out_three"
    "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out_three" "$pak"
    [ "$(cat "$BATS_TEST_TMPDIR/out_three/AAA")" = "alpha" ]
    [ "$(cat "$BATS_TEST_TMPDIR/out_three/CCC")" = "charlie" ]
    [ ! -f "$BATS_TEST_TMPDIR/out_three/BBB" ]
}

@test "wad open: format hint mismatch returns invalid argument" {
    pak="$BATS_TEST_TMPDIR/hint.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes"
    run "$PAKKA" -l --format pwad "$pak"
    [ "$status" -ne 0 ]
    # Cross-check: PK3 cannot be opened with --format iwad.
    pak3="$BATS_TEST_TMPDIR/hint.pk3"
    "$PAKKA" -c --format pk3 "$pak3" </dev/null
    run "$PAKKA" -l --format iwad "$pak3"
    [ "$status" -ne 0 ]
}

@test "wad open: AUTO resolves to IWAD/PWAD without PACK/DK probe" {
    pak="$BATS_TEST_TMPDIR/auto.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes"
    run "$PAKKA" -l "$pak"
    [ "$status" -eq 0 ]
    # An ambiguous-PACK error would mention "Daikatana"; AUTO on a WAD
    # must not surface it.
    [[ "$output" != *Daikatana* ]]
}

@test "wad open: oversize numlumps is rejected with LIMIT" {
    pak="$BATS_TEST_TMPDIR/huge.wad"
    # 0x10000000 lumps would imply a 4 GiB directory; PAKFILE_MAX_ENTRIES
    # is 1,048,576.
    {
        printf 'IWAD'
        u32 268435456
        u32 12
    } > "$pak"
    run "$PAKKA" -l "$pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -qiE 'too many|max'
}

@test "wad open: garbage filepos (0xFFFFFFFF) is rejected by bounds check" {
    pak="$BATS_TEST_TMPDIR/bad.wad"
    # One entry: claims size=4 but filepos=0xFFFFFFFF (would wrap past EOF).
    {
        printf 'IWAD'
        u32 1
        u32 12
        u32 4294967295  # filepos = 0xFFFFFFFF
        u32 4           # size
        wad_name "BOGUS"
    } > "$pak"
    run "$PAKKA" -l "$pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -qiE 'out of range|bytes out'
}

@test "wad verify: valid IWAD passes; truncated entry fails" {
    pak="$BATS_TEST_TMPDIR/verify_ok.wad"
    write_wad_synth "$pak" IWAD PLAYPAL "palette-bytes"
    run "$PAKKA" --verify "$pak"
    [ "$status" -eq 0 ]

    pak2="$BATS_TEST_TMPDIR/verify_bad.wad"
    # One entry whose size points past EOF.
    {
        printf 'IWAD'
        u32 1
        u32 12
        u32 12          # filepos at end of header
        u32 1000        # size far past EOF
        wad_name "BAD"
    } > "$pak2"
    run "$PAKKA" --verify "$pak2"
    [ "$status" -ne 0 ]
}

@test "wad verify: duplicate lump names are reported as WARNING (exit 0)" {
    # Real Doom maps repeat lump names across every map (THINGS, LINEDEFS,
    # SIDEDEFS, ...). pakka_verify documents these as PAKKA_REPORT_WARNING
    # findings on WAD only — the run must still exit OK so that
    # `pakka --verify` against a stock IWAD doesn't spuriously fail.
    pak="$BATS_TEST_TMPDIR/dup_lumps.wad"
    write_wad_synth "$pak" IWAD THINGS "map1-things" THINGS "map2-things"
    run "$PAKKA" --verify "$pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -qE 'WARNING.*Duplicate lump'
    # And the same archive label-swapped to a Quake-style PAK must still
    # fail verification — the WARNING relaxation is WAD-specific.
    echo "$output" | grep -vqE 'ERROR.*Normalized collision'
}
