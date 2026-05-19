#!/usr/bin/env bats

setup_file() {
    cd "$BATS_TEST_DIRNAME/.."
    : "${DK_CODEC_TEST:=build/test/dk_codec_test}"
    [ -x "$DK_CODEC_TEST" ] || skip "dk_codec_test binary missing at $DK_CODEC_TEST — run 'make dk_codec_test' first"
    export DK_CODEC_TEST
}

@test "dk codec: opcode boundaries, overlap, bounds, termination" {
    run "$DK_CODEC_TEST"
    [ "$status" -eq 0 ]
    [[ "$output" == *"All dk_codec_test scenarios passed."* ]]
}
