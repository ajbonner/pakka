#!/usr/bin/env bats
#
# Q3 demo pak0.pk3 fixture — gated behind `make realpak-test-q3`, not
# triggered by default `make test`. Pulls the 93 MiB wrapper from
# archive.org, SHA-pins it, and uses pakka itself to extract the
# inner pak0.pk3.
#
# The fixture path comes in via env var Q3DEMO_PAK0_PK3 (set by the
# realpak-test-q3 Makefile target). If absent, every test skips so this
# bats file is safe to include in a directory-wide `bats tests/`.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

setup_file() {
    if [ -z "$Q3DEMO_PAK0_PK3" ] || [ ! -f "$Q3DEMO_PAK0_PK3" ]; then
        export SKIP_ALL=1
    fi
}

skip_if_missing() {
    if [ -n "$SKIP_ALL" ]; then
        skip "Q3DEMO_PAK0_PK3 not set or not present — run via 'make realpak-test-q3'"
    fi
}

@test "q3demo: pakka lists 1274 entries" {
    skip_if_missing
    run "$PAKKA" -l "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]
    line_count="${#lines[@]}"
    [ "$line_count" -eq 1274 ]
}

@test "q3demo: --tree renders without error" {
    skip_if_missing
    run "$PAKKA" -l --tree "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]
    # Some recognizable Q3 asset paths must appear.
    [[ "$output" == *"models"* ]]
    [[ "$output" == *"sound"* ]]
}

@test "q3demo: structural verify succeeds (no findings above INFO)" {
    skip_if_missing
    run "$PAKKA" --verify "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]
    # No WARNING/ERROR lines.
    [[ "$output" != *"ERROR"* ]]
    [[ "$output" != *"WARNING"* ]]
}

@test "q3demo: deep verify (CRC32 every entry) succeeds" {
    skip_if_missing
    run "$PAKKA" --verify --deep "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]
    [[ "$output" != *"ERROR"* ]]
    [[ "$output" != *"WARNING"* ]]
}

@test "q3demo: extract every entry; spot-check a known asset byte count" {
    skip_if_missing
    rm -rf "$BATS_TEST_TMPDIR/out"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -x -C "$BATS_TEST_TMPDIR/out" "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]

    # Spot-check: vm/cgame.qvm exists and is the expected size.
    [ -f "$BATS_TEST_TMPDIR/out/vm/cgame.qvm" ]
    sz=$(wc -c < "$BATS_TEST_TMPDIR/out/vm/cgame.qvm")
    [ "$sz" -eq 236956 ]

    # Spot-check: scripts/base.shader exists with the expected size.
    [ -f "$BATS_TEST_TMPDIR/out/scripts/base.shader" ]
    sz=$(wc -c < "$BATS_TEST_TMPDIR/out/scripts/base.shader")
    [ "$sz" -eq 2247 ]
}

@test "q3demo: pakka_format() returns PAKKA_FORMAT_PK3" {
    skip_if_missing
    command -v ${CC:-cc} >/dev/null 2>&1 || skip "no cc in PATH (MSYS2 / MSVC build)"
    cat > "$BATS_TEST_TMPDIR/q3_fmt.c" <<'EOF'
#include <stdio.h>
#include "pakka.h"
int main(int argc, char **argv) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s = pakka_open(argv[1], PAKKA_OPEN_READ, &a, &err);
    if (s != PAKKA_OK) { fprintf(stderr, "open: %s\n", err.message); return 2; }
    size_t n = pakka_entry_count(a);
    pakka_format_t f = pakka_format(a);
    pakka_close(a, &err);
    if (f != PAKKA_FORMAT_PK3) return 3;
    if (n != 1274) {
        fprintf(stderr, "expected 1274 entries, got %zu\n", n);
        return 4;
    }
    return 0;
}
EOF
    ${CC:-cc} ${CFLAGS:-} -I"${PROJECT_ROOT}/include" \
        -o "$BATS_TEST_TMPDIR/q3_fmt" \
        "$BATS_TEST_TMPDIR/q3_fmt.c" \
        "${LIBPAKKA:-${PROJECT_ROOT}/build/lib-prod/libpakka.a}"
    run "$BATS_TEST_TMPDIR/q3_fmt" "$Q3DEMO_PAK0_PK3"
    [ "$status" -eq 0 ]
}
