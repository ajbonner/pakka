#!/usr/bin/env bats
#
# Integration tests for pakka against id's shareware Quake pak0.pak.
# Make ensures pakka is built and pak0.pak is downloaded+verified
# before bats runs. Each @test gets its own $BATS_TEST_TMPDIR (auto-cleaned);
# $BATS_FILE_TMPDIR survives across tests for once-per-file fixtures.
#
# Note on pak0.pak: id's pak ships with 432,312 bytes of internal gaps —
# its directory sits mid-file and a 410,616-byte orphan progs.dat lives
# in a hole left when id appended a recompiled progs.dat without rewriting
# the pak. pakka's pack produces a compact layout, so byte-level comparisons
# against the original differ by exactly 410,616 bytes (locked in by test 3).
# Tests that care about a clean baseline use rebuilt.pak (produced once in
# setup_file) rather than pak0.pak directly.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PROJECT_ROOT}/pakka"
PAK0="${PROJECT_ROOT}/build/test/pak0.pak"

setup_file() {
    mkdir -p "$BATS_FILE_TMPDIR/extracted"
    "$PAKKA" -xf "$PAK0" -C "$BATS_FILE_TMPDIR/extracted" >/dev/null
    (cd "$BATS_FILE_TMPDIR/extracted" && "$PAKKA" -cf "$BATS_FILE_TMPDIR/rebuilt.pak" *) >/dev/null
}

@test "list: pak0.pak contains 339 entries" {
    run "$PAKKA" -lf "$PAK0"
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | wc -l | tr -d ' ')" -eq 339 ]
}

@test "round-trip: extract -> repack -> re-extract is content-identical" {
    mkdir -p "$BATS_TEST_TMPDIR/re_extracted"
    "$PAKKA" -xf "$BATS_FILE_TMPDIR/rebuilt.pak" -C "$BATS_TEST_TMPDIR/re_extracted" >/dev/null
    diff -rq "$BATS_FILE_TMPDIR/extracted" "$BATS_TEST_TMPDIR/re_extracted"
}

@test "round-trip: byte delta vs original matches expected orphan size" {
    # Locks in the 410,616-byte invariant described at the top of this file.
    # If this fails: either pak0.pak changed upstream (caught earlier by the
    # tarball SHA256 in the Makefile) or pakka's pack layout changed.
    orig=$(wc -c < "$PAK0" | tr -d ' ')
    new=$(wc -c < "$BATS_FILE_TMPDIR/rebuilt.pak" | tr -d ' ')
    [ $((orig - new)) -eq 410616 ]
}

@test "extract: specific files only" {
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$PAK0" -C "$BATS_TEST_TMPDIR/out" default.cfg quake.rc >/dev/null
    [ "$(find "$BATS_TEST_TMPDIR/out" -type f | wc -l | tr -d ' ')" -eq 2 ]
    cmp -s "$BATS_FILE_TMPDIR/extracted/default.cfg" "$BATS_TEST_TMPDIR/out/default.cfg"
    cmp -s "$BATS_FILE_TMPDIR/extracted/quake.rc" "$BATS_TEST_TMPDIR/out/quake.rc"
}

@test "add: new entry appears in listing and round-trips" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    echo "pakka crud test payload" > "$BATS_TEST_TMPDIR/added.txt"
    (cd "$BATS_TEST_TMPDIR" && "$PAKKA" -af work.pak added.txt) >/dev/null

    "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | grep -q '^added\.txt '

    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/out" added.txt >/dev/null
    cmp -s "$BATS_TEST_TMPDIR/added.txt" "$BATS_TEST_TMPDIR/out/added.txt"
}

@test "delete: head entry + tail entry, untouched files survive intact" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"

    head_entry=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | head -1 | awk '{print $1}')
    tail_entry=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | tail -1 | awk '{print $1}')
    [ -n "$head_entry" ]
    [ -n "$tail_entry" ]
    [ "$head_entry" != "$tail_entry" ]

    before=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | wc -l | tr -d ' ')

    "$PAKKA" -df "$BATS_TEST_TMPDIR/work.pak" "$head_entry" "$tail_entry" >/dev/null

    after=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | wc -l | tr -d ' ')
    [ $((before - after)) -eq 2 ]

    # Use awk for literal filename comparison; filenames could theoretically
    # contain regex metacharacters (.+ etc.) that would mislead grep.
    "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" \
        | awk -v name="$head_entry" '$1 == name {found=1} END {exit found}'
    "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" \
        | awk -v name="$tail_entry" '$1 == name {found=1} END {exit found}'

    # An untouched file should still extract byte-identically.
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/out" >/dev/null
    cmp -s "$BATS_FILE_TMPDIR/extracted/demo1.dem" "$BATS_TEST_TMPDIR/out/demo1.dem"
}

@test "delete: removing every entry leaves a valid empty pak" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"

    all_paths=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | awk '{print $1}')
    # shellcheck disable=SC2086  # intentional word-splitting for the path list
    "$PAKKA" -df "$BATS_TEST_TMPDIR/work.pak" $all_paths >/dev/null

    # File must still be exactly a valid empty PACK header (12 bytes).
    [ "$(wc -c < "$BATS_TEST_TMPDIR/work.pak" | tr -d ' ')" -eq 12 ]
    [ "$(head -c 4 "$BATS_TEST_TMPDIR/work.pak")" = "PACK" ]

    # Directly check diroffset (bytes 4-7, LE u32) == 12. dirlength == 0 is
    # implied by the file being exactly 12 bytes long (and confirmed via -lf).
    diroffset=$(od -An -t u4 -N4 -j4 "$BATS_TEST_TMPDIR/work.pak" | tr -d ' ')
    [ "$diroffset" -eq 12 ]

    # Listing should succeed and report empty rather than crashing.
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Pak is empty"

    # Belt and braces: extracting an empty pak should be a clean no-op
    # (exit 0, no files written) and asking for any specific path must error.
    mkdir -p "$BATS_TEST_TMPDIR/empty_out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/empty_out"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Pak is empty"
    [ "$(find "$BATS_TEST_TMPDIR/empty_out" -type f | wc -l | tr -d ' ')" -eq 0 ]

    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/empty_out" any-file
    [ "$status" -ne 0 ]
}

@test "extract: requesting a missing path errors out" {
    run "$PAKKA" -xf "$PAK0" -C "$BATS_TEST_TMPDIR/out" no-such-file-in-pak
    [ "$status" -ne 0 ]
}

@test "extract: duplicate path arguments succeed (regression: don't break early)" {
    # `default.cfg default.cfg` should extract once and not error on the
    # duplicate. The earlier path-match loop broke after the first hit,
    # leaving path_matched[1] zero and triggering a false "not found".
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$PAK0" -C "$BATS_TEST_TMPDIR/out" default.cfg default.cfg >/dev/null
    [ -f "$BATS_TEST_TMPDIR/out/default.cfg" ]
    cmp -s "$BATS_FILE_TMPDIR/extracted/default.cfg" "$BATS_TEST_TMPDIR/out/default.cfg"
}

@test "open: pak with only PACK magic is rejected (load_pakfile fread check)" {
    # Exercises the load_pakfile fread checks. Pak_t is calloc'd, so without
    # those checks a short read would leave diroffset/dirlength at zero and
    # the file would parse as a (bogus) empty pak instead of erroring.
    printf 'PACK' > "$BATS_TEST_TMPDIR/truncated.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/truncated.pak"
    [ "$status" -ne 0 ]
}

@test "open: pak with truncated directory is rejected (load_directory fread check)" {
    # Header claims 1 entry (dirlength=64) starting at offset 12, but the
    # file ends at byte 12. load_directory's fread should catch the short
    # read and error_exit instead of silently producing a zeroed entry.
    # Bytes: 'PACK' + diroffset=12 (LE u32) + dirlength=64 (LE u32) = 12 bytes total.
    printf 'PACK\014\000\000\000\100\000\000\000' > "$BATS_TEST_TMPDIR/short_dir.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/short_dir.pak"
    [ "$status" -ne 0 ]
}
