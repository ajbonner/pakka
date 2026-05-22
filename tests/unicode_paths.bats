#!/usr/bin/env bats
#
# Unicode / non-ASCII path handling tests. These primarily exercise
# the Windows fix (UTF-8 ↔ UTF-16 conversion at the syscall boundary,
# wmain wrapper, ZIP GPBF bit 11, CP437 fallback, invalid-UTF-8 extract
# substitution). On POSIX the same code paths are exercised but with
# the host treating UTF-8 bytes opaquely — also valuable, so these
# tests are not skipped on POSIX. See dev/docs/windows-codepage.md.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# UTF-8 bytes for "Тест.txt" (Cyrillic "Test.txt").
#   Т U+0422 → D0 A2
#   е U+0435 → D0 B5
#   с U+0441 → D1 81
#   т U+0442 → D1 82
CYR_UTF8=$'\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82.txt'

# Same characters in CP1251 bytes (legacy Russian PC encoding). NOT
# valid UTF-8 (0xD2 starts a 2-byte sequence but is followed by 0xE5
# which has bit 7 set but bit 6 clear — invalid continuation).
CYR_CP1251=$'\xD2\xE5\xF1\xF2.txt'

# Build a minimal Quake PAK with a single entry whose name is the raw
# bytes provided as $2 (no UTF-8 / encoding interpretation — pakka's
# pre-fix behavior). 12-byte header + 64-byte dir entry + 0-byte payload.
# Used to inject a legacy CP1251 name for the extract-fallback test.
write_pak_one_entry() {
    local pakfile="$1" name="$2"
    local pad=$((56 - ${#name}))
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf '%s' "$name"
        dd if=/dev/zero bs=1 count="$pad" 2>/dev/null
        printf '\114\000\000\000\000\000\000\000'
    } > "$pakfile"
}

@test "unicode pak: add+list round-trips a Cyrillic UTF-8 filename" {
    cd "$BATS_TEST_TMPDIR"
    printf 'hello' > "$CYR_UTF8"
    "$PAKKA" -c out.pak "$CYR_UTF8"
    run "$PAKKA" -l out.pak
    [ "$status" -eq 0 ]
    [[ "$output" == *"$CYR_UTF8"* ]]
}

@test "unicode pak: archive header stores raw UTF-8 bytes for Cyrillic name" {
    cd "$BATS_TEST_TMPDIR"
    printf 'hello' > "$CYR_UTF8"
    "$PAKKA" -c out.pak "$CYR_UTF8"
    # Header (12 bytes) + directory offset (4 LE bytes at offset 4)
    # then the directory entry starts with the 56-byte name field. We
    # don't bother parsing the offset; pakka always writes the directory
    # contiguous after a single payload, so we just grep for the bytes.
    od -An -vtx1 out.pak | tr -d ' \n' | grep -qi 'd0a2d0b5d181d182'
}

@test "unicode pak: extract round-trips a Cyrillic UTF-8 filename" {
    cd "$BATS_TEST_TMPDIR"
    mkdir extracted
    printf 'hello' > "$CYR_UTF8"
    "$PAKKA" -c out.pak "$CYR_UTF8"
    "$PAKKA" -x -C extracted out.pak
    [ -f "extracted/$CYR_UTF8" ]
    [ "$(cat "extracted/$CYR_UTF8")" = "hello" ]
}

@test "unicode pk3: GPBF bit 11 set in LFH and CDR for Cyrillic name" {
    cd "$BATS_TEST_TMPDIR"
    printf 'hello' > "$CYR_UTF8"
    "$PAKKA" -c out.pk3 "$CYR_UTF8"
    # LFH: 'P' 'K' \003 \004, GP flags at LFH offset +6 (2 bytes LE).
    # CDR: 'P' 'K' \001 \002, GP flags at CDR offset +8 (2 bytes LE).
    # bit 11 = 0x0800 → the low GP-flag byte is 0x00 and the high
    # byte has 0x08 set. We extract and check both.
    local hex
    hex=$(od -An -vtx1 out.pk3 | tr -d ' \n')
    # LFH at offset 0 in pakka's output (no pre-header data). GP flags
    # bytes at positions 12-15 (hex chars 12..15) ... LFH offset 6 = hex
    # chars 12..13 (low byte) and 14..15 (high byte).
    local lfh_gp_lo lfh_gp_hi
    lfh_gp_lo="${hex:12:2}"
    lfh_gp_hi="${hex:14:2}"
    [ "$lfh_gp_lo" = "00" ]
    [ "$lfh_gp_hi" = "08" ]
    # Locate the CDR signature ("504b0102") and check GP flags at CDR+8
    # (hex chars at CDR_pos+16 / CDR_pos+18). Use bash parameter
    # expansion to find the prefix length — BusyBox grep (Alpine /
    # musl CI) does not support `grep -b`.
    local cdr_pos cdr_gp_lo cdr_gp_hi prefix
    prefix="${hex%%504b0102*}"
    [ "$prefix" != "$hex" ]
    cdr_pos=${#prefix}
    cdr_gp_lo="${hex:$((cdr_pos + 16)):2}"
    cdr_gp_hi="${hex:$((cdr_pos + 18)):2}"
    [ "$cdr_gp_lo" = "00" ]
    [ "$cdr_gp_hi" = "08" ]
}

@test "unicode pk3: pure-ASCII names leave GPBF bit 11 clear" {
    cd "$BATS_TEST_TMPDIR"
    printf 'hello' > plain.txt
    "$PAKKA" -c out.pk3 plain.txt
    local hex lfh_gp_hi
    hex=$(od -An -vtx1 out.pk3 | tr -d ' \n')
    lfh_gp_hi="${hex:14:2}"
    # bit 11 (0x08) must be clear; the GP flag field should be 0.
    [ "$lfh_gp_hi" = "00" ]
}

@test "legacy pak: extract substitutes invalid UTF-8 bytes and warns" {
    cd "$BATS_TEST_TMPDIR"
    write_pak_one_entry legacy.pak "$CYR_CP1251"
    mkdir out
    run "$PAKKA" -x -C out legacy.pak
    [ "$status" -eq 0 ]
    # Stderr warning naming the entry (index 0) and the substituted
    # form ('_'-replaced bytes followed by '.txt').
    [[ "$output" == *"not valid UTF-8"* ]]
    # A file named "____.txt" should exist (4 invalid bytes → 4 '_').
    [ -f "out/____.txt" ]
}

@test "list: invalid UTF-8 entry name renders without TTY corruption" {
    cd "$BATS_TEST_TMPDIR"
    write_pak_one_entry legacy.pak "$CYR_CP1251"
    run "$PAKKA" -l legacy.pak
    [ "$status" -eq 0 ]
    # pakka_fprint_sanitized should have ?-substituted the invalid
    # byte runs so the listing line is pure ASCII printable.
    printf '%s\n' "$output" | LC_ALL=C grep -E '^[[:print:]]+$' >/dev/null
}

# Build a minimal valid PAK with TWO entries whose 56-byte name fields
# both hold raw bytes (not validated as UTF-8). Used to test the
# collision check after substitution.
write_pak_two_entries() {
    local pakfile="$1" name_a="$2" name_b="$3"
    local pad_a=$((56 - ${#name_a}))
    local pad_b=$((56 - ${#name_b}))
    {
        # PACK + diroffset=12 (LE) + dirlength=128 (LE = two 64-byte entries)
        printf 'PACK\014\000\000\000\200\000\000\000'
        # entry A: name + pad + offset=140 + length=0
        printf '%s' "$name_a"
        dd if=/dev/zero bs=1 count="$pad_a" 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        # entry B: name + pad + offset=140 + length=0
        printf '%s' "$name_b"
        dd if=/dev/zero bs=1 count="$pad_b" 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$pakfile"
}

@test "legacy pak: collision on sanitized name is detected, not silently overwritten" {
    cd "$BATS_TEST_TMPDIR"
    # Two CP1251 names that BOTH become "____.txt" after invalid-UTF-8
    # substitution: \xD2\xE5\xF1\xF2 ("Тест" in CP1251) and
    # \xC0\xC1\xC2\xC3 ("АБВГ" in CP1251). Different on-disk bytes,
    # both 4-byte invalid-UTF-8 prefixes followed by ".txt".
    local name_a name_b
    name_a=$'\xD2\xE5\xF1\xF2.txt'
    name_b=$'\xC0\xC1\xC2\xC3.txt'
    write_pak_two_entries legacy.pak "$name_a" "$name_b"
    mkdir out
    run "$PAKKA" -x -C out legacy.pak
    # Must NOT exit successfully — silently writing both as "____.txt"
    # would drop one entry's payload.
    [ "$status" -ne 0 ]
    [[ "$output" == *"collide"* ]]
}

# Build a minimal ZIP/PK3 with a single entry whose name bytes and GP
# flag word can be chosen independently. Used to test the bit-11 +
# invalid-UTF-8 rejection.
write_pk3_one_entry() {
    local pk3file="$1" name="$2" gp_lo="$3" gp_hi="$4"
    local name_len="${#name}"
    local name_len_lo=$((name_len & 0xFF))
    local name_len_hi=$(((name_len >> 8) & 0xFF))
    # LFH (30 bytes) + name + (no payload, no extra) +
    # CDR (46 bytes) + name + EOCD (22 bytes)
    python3 -c "
import sys, struct
name = sys.argv[1].encode('latin-1', 'surrogateescape')
gp = int(sys.argv[2])
lfh = struct.pack('<4sHHHHHIIIHH', b'PK\x03\x04', 20, gp, 0, 0, 0x0021,
                  0, 0, 0, len(name), 0)
sys.stdout.buffer.write(lfh + name)
lfh_off = 0
cdr_off = len(lfh) + len(name)
cdr = struct.pack('<4sHHHHHHIIIHHHHHII', b'PK\x01\x02', 20, 20, gp, 0,
                  0, 0x0021, 0, 0, 0, len(name), 0, 0, 0, 0, 0, lfh_off)
sys.stdout.buffer.write(cdr + name)
eocd = struct.pack('<4sHHHHIIH', b'PK\x05\x06', 0, 0, 1, 1,
                   len(cdr) + len(name), cdr_off, 0)
sys.stdout.buffer.write(eocd)
" "$name" "$(( (gp_hi << 8) | gp_lo ))" > "$pk3file"
}

@test "pk3 open: rejects entry with bit 11 set but invalid UTF-8 name" {
    cd "$BATS_TEST_TMPDIR"
    command -v python3 >/dev/null 2>&1 || skip "python3 not available for ZIP fixture build"
    # CP1251 bytes "Тест.txt" — definitely not valid UTF-8 — but with
    # GP bit 11 (0x0800) advertising UTF-8.
    write_pk3_one_entry bogus.pk3 $'\xD2\xE5\xF1\xF2.txt' 0x00 0x08
    run "$PAKKA" -l bogus.pk3
    [ "$status" -ne 0 ]
    [[ "$output" == *"UTF-8 flag"* ]] || [[ "$output" == *"not valid UTF-8"* ]]
}

@test "pk3 open: bit-11-clear CP437 name decodes via fallback table" {
    cd "$BATS_TEST_TMPDIR"
    command -v python3 >/dev/null 2>&1 || skip "python3 not available for ZIP fixture build"
    # 0xE1 in CP437 is U+00DF (ß). Encode as a one-byte name field
    # with bit 11 CLEAR; pakka should decode to UTF-8 "ß" (\xC3\x9F).
    write_pk3_one_entry cp437.pk3 $'\xE1' 0x00 0x00
    run "$PAKKA" -l cp437.pk3
    [ "$status" -eq 0 ]
    # Output should contain the UTF-8 encoding of ß.
    printf '%s' "$output" | LC_ALL=C grep -qF $'\xC3\x9F'
}
