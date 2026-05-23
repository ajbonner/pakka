#!/usr/bin/env bats
#
# GoldSrc PAK parity-confirmation fixtures — gated behind
# `make realpak-test-goldsrc`, not triggered by default `make test`.
# GoldSrc PAK is bit-identical to Quake/Q2 PAK at the format level
# (same "PACK" magic, 56-byte filename field, 64-byte directory entry),
# so these tests don't exercise a new code path — they prove the
# existing PAK reader handles real Valve-authored archives end-to-end.
#
# Two fixtures, each independently env-var gated:
#   GOLDSRC_UPLINK_PAK0   — Half-Life Uplink (1999 free demo) valve/pak0.pak
#   GOLDSRC_DAYONE_PAK0   — Half-Life: Day One (1998 OEM demo)  valve/pak0.pak
#
# Both are set by the realpak-test-goldsrc Makefile target; if absent,
# the corresponding tests skip so `bats test/` remains safe to run.
#
# Pinned entry counts and asset sizes are tied to the SHA-pinned
# wrappers in the Makefile. If you re-pin the wrapper SHAs (e.g. a
# new archive.org mirror), recompute the counts here too:
#   pakka -l <pak> | wc -l
#   wc -c <extracted-asset>

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

setup_file() {
    if [ -z "$GOLDSRC_UPLINK_PAK0" ] || [ ! -f "$GOLDSRC_UPLINK_PAK0" ]; then
        export SKIP_UPLINK=1
    fi
    if [ -z "$GOLDSRC_DAYONE_PAK0" ] || [ ! -f "$GOLDSRC_DAYONE_PAK0" ]; then
        export SKIP_DAYONE=1
    fi
}

skip_if_uplink_missing() {
    if [ -n "$SKIP_UPLINK" ]; then
        skip "GOLDSRC_UPLINK_PAK0 not set or not present — run via 'make realpak-test-goldsrc'"
    fi
}

skip_if_dayone_missing() {
    if [ -n "$SKIP_DAYONE" ]; then
        skip "GOLDSRC_DAYONE_PAK0 not set or not present — run via 'make realpak-test-goldsrc'"
    fi
}

# ----- Half-Life Uplink fixture -----

@test "goldsrc/uplink: pakka -l reports 1952 entries" {
    skip_if_uplink_missing
    run "$PAKKA" -l "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1952 ]
}

@test "goldsrc/uplink: pakka -l --format goldsrc is identical to --format pak" {
    skip_if_uplink_missing
    # Assert each invocation succeeds before asserting equality —
    # otherwise three identically-failed commands with empty stdout
    # would pass the equality check trivially.
    run "$PAKKA" -l --format pak "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    pak_out="$output"
    run "$PAKKA" -l --format goldsrc "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    gs_out="$output"
    run "$PAKKA" -l --format hl "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    hl_out="$output"
    [ "$pak_out" = "$gs_out" ]
    [ "$pak_out" = "$hl_out" ]
}

@test "goldsrc/uplink: --tree renders without error" {
    skip_if_uplink_missing
    run "$PAKKA" -l --tree "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    # Valve's GoldSrc paks always nest assets under maps/, models/, sound/, sprites/.
    [[ "$output" == *"maps"* ]]
    [[ "$output" == *"models"* ]]
}

@test "goldsrc/uplink: structural verify succeeds (no findings above INFO)" {
    skip_if_uplink_missing
    run "$PAKKA" --verify "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    [[ "$output" != *"ERROR"* ]]
    [[ "$output" != *"WARNING"* ]]
}

@test "goldsrc/uplink: full extract materializes the GoldSrc asset families" {
    skip_if_uplink_missing
    rm -rf "$BATS_TEST_TMPDIR/uplink-out"
    mkdir -p "$BATS_TEST_TMPDIR/uplink-out"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/uplink-out" "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
    # GoldSrc-typical asset families must materialize on disk.
    [ -d "$BATS_TEST_TMPDIR/uplink-out/maps" ]
    [ -d "$BATS_TEST_TMPDIR/uplink-out/models" ]
    # Uplink's tram-ride opener map.
    sz=$(wc -c < "$BATS_TEST_TMPDIR/uplink-out/maps/t0a0.bsp")
    [ "$sz" -eq 3003032 ]
}

@test "goldsrc/uplink: delete + rebuild drops exactly the victim entry" {
    skip_if_uplink_missing
    cp "$GOLDSRC_UPLINK_PAK0" "$BATS_TEST_TMPDIR/uplink-mut.pak"

    # Snapshot the listing before and after the delete; confirm the
    # victim is the only entry that disappeared. This is stronger than
    # "count went down by one" — it catches a delete that removes the
    # wrong entry or a rebuild that drops more than the named victim.
    #
    # `tr -d '\r'` normalizes line endings: the MSVC-built pakka.exe
    # emits CRLF on stdout, and MSYS2 grep strips the CR on output —
    # so without normalization the `expected` capture (filtered via
    # grep) ends up LF-only while `after_list` (direct from pakka.exe)
    # keeps CRLF, and every line's terminator mismatches. No-op on Unix.
    before_list=$("$PAKKA" -l "$BATS_TEST_TMPDIR/uplink-mut.pak" | tr -d '\r')
    before=$(echo "$before_list" | wc -l | tr -d ' ')
    # pakka -l renders "entry/name (NNN bytes)" — strip the byte suffix.
    victim=$(echo "$before_list" | head -n 1 | awk '{print $1}')
    expected=$(echo "$before_list" | grep -vF "$victim " || true)

    run "$PAKKA" -d "$BATS_TEST_TMPDIR/uplink-mut.pak" "$victim"
    [ "$status" -eq 0 ]

    after_list=$("$PAKKA" -l "$BATS_TEST_TMPDIR/uplink-mut.pak" | tr -d '\r')
    after=$(echo "$after_list" | wc -l | tr -d ' ')
    [ "$((before - after))" -eq 1 ]
    [ "$after_list" = "$expected" ]

    run "$PAKKA" --verify "$BATS_TEST_TMPDIR/uplink-mut.pak"
    [ "$status" -eq 0 ]
}

@test "goldsrc/uplink: pakka_format() returns PAKKA_FORMAT_PAK" {
    skip_if_uplink_missing
    command -v ${CC:-cc} >/dev/null 2>&1 || skip "no cc in PATH (MSYS2 / MSVC build)"
    cat > "$BATS_TEST_TMPDIR/gs_fmt.c" <<'EOF'
#include <stdio.h>
#include "pakka.h"
int main(int argc, char **argv) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s = pakka_open(argv[1], PAKKA_OPEN_READ, &a, &err);
    if (s != PAKKA_OK) { fprintf(stderr, "open: %s\n", err.message); return 2; }
    pakka_format_t f = pakka_format(a);
    pakka_close(a, &err);
    return (f == PAKKA_FORMAT_PAK) ? 0 : 3;
}
EOF
    ${CC:-cc} ${CFLAGS:-} -I"${PROJECT_ROOT}/include" \
        -o "$BATS_TEST_TMPDIR/gs_fmt" \
        "$BATS_TEST_TMPDIR/gs_fmt.c" \
        "${LIBPAKKA:-${PROJECT_ROOT}/build/lib-prod/libpakka.a}"
    run "$BATS_TEST_TMPDIR/gs_fmt" "$GOLDSRC_UPLINK_PAK0"
    [ "$status" -eq 0 ]
}

# ----- Half-Life: Day One fixture -----

@test "goldsrc/dayone: pakka -l reports 2598 entries" {
    skip_if_dayone_missing
    run "$PAKKA" -l "$GOLDSRC_DAYONE_PAK0"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 2598 ]
}

@test "goldsrc/dayone: --format goldsrc is identical to --format pak" {
    skip_if_dayone_missing
    # Same pattern as the Uplink alias test — assert each invocation
    # succeeds + produces non-empty output before comparing, so two
    # identically-failed-empty commands can't trivially pass.
    run "$PAKKA" -l --format pak "$GOLDSRC_DAYONE_PAK0"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    pak_out="$output"
    run "$PAKKA" -l --format goldsrc "$GOLDSRC_DAYONE_PAK0"
    [ "$status" -eq 0 ]
    gs_out="$output"
    [ "$pak_out" = "$gs_out" ]
}

@test "goldsrc/dayone: structural verify succeeds" {
    skip_if_dayone_missing
    run "$PAKKA" --verify "$GOLDSRC_DAYONE_PAK0"
    [ "$status" -eq 0 ]
    [[ "$output" != *"ERROR"* ]]
    [[ "$output" != *"WARNING"* ]]
}

@test "goldsrc/dayone: extract every entry; spot-check maps/c0a0.bsp" {
    skip_if_dayone_missing
    rm -rf "$BATS_TEST_TMPDIR/dayone-out"
    mkdir -p "$BATS_TEST_TMPDIR/dayone-out"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/dayone-out" "$GOLDSRC_DAYONE_PAK0"
    [ "$status" -eq 0 ]
    [ -d "$BATS_TEST_TMPDIR/dayone-out/maps" ]
    # Black Mesa monorail tram intro.
    sz=$(wc -c < "$BATS_TEST_TMPDIR/dayone-out/maps/c0a0.bsp")
    [ "$sz" -eq 2232604 ]
}
