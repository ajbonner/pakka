#!/usr/bin/env bats
#
# Large-file regression for the 32-bit fseek/ftell ceiling. Synthesizes
# a sparse pak whose directory sits above LONG_MAX (2,147,483,647) so
# the wide-seek path is actually exercised. Mainly proves the 32-bit
# CI jobs (linux-glibc-amd64-m32, freebsd-amd64-m32); 64-bit hosts pass
# regardless, but locking in the behavior across the matrix is cheap.
# Skips when `truncate(1)` is missing, which is how Windows MSYS2 lands
# without the GNU coreutils package.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# Both offsets above LONG_MAX so the wide-seek path actually fires on
# 32-bit hosts.
LARGE_FILE_SIZE=2600000000
LARGE_DIR_OFFSET=2500000000

# Build a single-entry pak in $1 whose directory sits at
# $LARGE_DIR_OFFSET. Payload "data" lives at offset 12 (just past the
# 12-byte header); the rest is a sparse hole. Entry name is "tiny"
# followed by NULs out to 56 bytes (the on-disk format field width).
make_large_pak() {
    local pakfile="$1"

    # `truncate -s` extends the logical size past LARGE_DIR_OFFSET as
    # a sparse hole; the subsequent dd writes position into that hole
    # via seek=N conv=notrunc.
    truncate -s "$LARGE_FILE_SIZE" "$pakfile"

    # Header at offset 0:
    #   "PACK" + diroffset (LE u32 = 2_500_000_000 = 0x9502F900)
    #         + dirlength (LE u32 = 64)
    # 2_500_000_000 LE bytes: \000\371\002\225 (octal escapes —
    # 0xF9 = 0o371, 0x95 = 0o225). \100 = 64.
    printf 'PACK\000\371\002\225\100\000\000\000' \
        | dd of="$pakfile" bs=1 seek=0 count=12 conv=notrunc 2>/dev/null

    # Payload at offset 12.
    printf 'data' | dd of="$pakfile" bs=1 seek=12 count=4 conv=notrunc 2>/dev/null

    # Directory entry at offset $LARGE_DIR_OFFSET:
    #   56-byte filename "tiny\0...\0" + offset=12 (LE) + length=4 (LE)
    {
        printf 'tiny'
        dd if=/dev/zero bs=1 count=52 2>/dev/null
        printf '\014\000\000\000\004\000\000\000'
    } | dd of="$pakfile" bs=1 seek="$LARGE_DIR_OFFSET" count=64 conv=notrunc 2>/dev/null
}

setup_file() {
    command -v truncate >/dev/null 2>&1 \
        || skip "truncate(1) not available; can't synthesize sparse pak"

    # One sparse pak per file — every @test reads from the same path.
    LARGE_PAK="$BATS_FILE_TMPDIR/large.pak"
    export LARGE_PAK
    make_large_pak "$LARGE_PAK"
}

setup() {
    command -v truncate >/dev/null 2>&1 \
        || skip "truncate(1) not available; can't synthesize sparse pak"
}

@test "large file: lists the single high-offset entry" {
    run "$PAKKA" -l "$LARGE_PAK"
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | wc -l | tr -d ' ')" -eq 1 ]
    echo "$output" | grep -q '^tiny '
}

@test "large file: extract recovers the payload bytes" {
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out" "$LARGE_PAK"
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/out/tiny" ]
    [ "$(wc -c < "$BATS_TEST_TMPDIR/out/tiny" | tr -d ' ')" -eq 4 ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/tiny")" = "data" ]
}

@test "large file: --verify passes" {
    run "$PAKKA" --verify "$LARGE_PAK"
    [ "$status" -eq 0 ]
}
