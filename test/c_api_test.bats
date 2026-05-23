#!/usr/bin/env bats

setup_file() {
    cd "$BATS_TEST_DIRNAME/.."
    : "${PAK0:=build/test/pak0.pak}"
    : "${C_API_TEST:=build/test/c_api_test}"

    # The pak0 fixture is downloaded by `make $(PAK0)`; the c_api_test
    # binary is built by `make c_api_test`. The Makefile's `test`
    # target already depends on both; this skip-guard lets you run
    # `bats test/` manually without building first.
    [ -f "$PAK0" ] || skip "pak0 fixture missing at $PAK0 — run 'make $(basename $PAK0)' first"
    [ -x "$C_API_TEST" ] || skip "c_api_test binary missing at $C_API_TEST — run 'make c_api_test' first"

    SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/pakka-c-api.XXXXXX")"
    export PAK0 C_API_TEST SCRATCH
}

teardown_file() {
    if [ -n "$SCRATCH" ] && [ -d "$SCRATCH" ]; then
        rm -rf "$SCRATCH"
    fi
}

@test "c-api: NULL tolerance, round-trips, accessors, verify, memory APIs" {
    run "$C_API_TEST" "$PAK0" "$SCRATCH"
    [ "$status" -eq 0 ]
    [[ "$output" == *"c_api_test: OK"* ]]
}
