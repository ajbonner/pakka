#!/usr/bin/env bats
#
# PK4 (Doom 3) container support. PK4 is byte-identical to PK3 on disk;
# this suite covers the format-labeling layer (CLI extension sniff, open-
# side label-from-extension, ZIP create dispatch) and the one bats-level
# gap codex flagged: the delete/rebuild commit path under a PK4 label.
# Synthetic fixtures only — built with /usr/bin/zip and pakka itself.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

setup_file() {
    mkdir -p "$BATS_FILE_TMPDIR/src/sub"
    echo -n "hello pk4" > "$BATS_FILE_TMPDIR/src/hello.txt"
    echo "nested" > "$BATS_FILE_TMPDIR/src/sub/nested.txt"
    # Compressible payload large enough for zip to pick DEFLATE — the
    # main reason a real-world Doom 3 PK4 differs from a fresh-made PK3.
    python3 -c "open('$BATS_FILE_TMPDIR/src/lorem.txt','w').write(('The quick brown fox jumps over the lazy dog. ' * 250))"
    (cd "$BATS_FILE_TMPDIR/src" && zip -q9 "$BATS_FILE_TMPDIR/mixed.pk4" -r .)
}

@test "pk4 list: enumerates entries in a /usr/bin/zip-built .pk4" {
    run "$PAKKA" -lf "$BATS_FILE_TMPDIR/mixed.pk4"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello.txt"* ]]
    [[ "$output" == *"sub/nested.txt"* ]]
    [[ "$output" == *"lorem.txt"* ]]
}

@test "pk4 extract: DEFLATE round-trips through pakka's reader" {
    rm -rf "$BATS_TEST_TMPDIR/out"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_FILE_TMPDIR/mixed.pk4" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -eq 0 ]
    diff -r "$BATS_FILE_TMPDIR/src" "$BATS_TEST_TMPDIR/out"
}

@test "pk4 verify: synthetic PK4 passes deep checks" {
    run "$PAKKA" --verify -f "$BATS_FILE_TMPDIR/mixed.pk4"
    [ "$status" -eq 0 ]
}

@test "pk4 create: -c .pk4 builds a valid ZIP and round-trips" {
    rm -rf "$BATS_TEST_TMPDIR/build_src" "$BATS_TEST_TMPDIR/build_out"
    mkdir -p "$BATS_TEST_TMPDIR/build_src/d" "$BATS_TEST_TMPDIR/build_out"
    echo "a" > "$BATS_TEST_TMPDIR/build_src/a.txt"
    echo "b nested" > "$BATS_TEST_TMPDIR/build_src/d/b.txt"

    pushd "$BATS_TEST_TMPDIR/build_src" >/dev/null
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/built.pk4" a.txt d
    popd >/dev/null
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/built.pk4" ]

    # PK\003\004 magic: pakka picked the ZIP create path from the .pk4
    # extension. Confirms the CLI extension sniffer recognises .pk4.
    sig=$(dd if="$BATS_TEST_TMPDIR/built.pk4" bs=1 count=4 2>/dev/null \
            | od -An -tx1 | tr -d ' \n')
    [ "$sig" = "504b0304" ]

    # External reader confirms ZIP validity.
    run unzip -t "$BATS_TEST_TMPDIR/built.pk4"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No errors detected"* ]]

    "$PAKKA" -xf "$BATS_TEST_TMPDIR/built.pk4" -C "$BATS_TEST_TMPDIR/build_out"
    diff -r "$BATS_TEST_TMPDIR/build_src" "$BATS_TEST_TMPDIR/build_out"
}

@test "pk4 create: uppercase .PK4 extension is also recognised" {
    rm -rf "$BATS_TEST_TMPDIR/upper_src"
    mkdir -p "$BATS_TEST_TMPDIR/upper_src"
    echo "x" > "$BATS_TEST_TMPDIR/upper_src/x.txt"

    pushd "$BATS_TEST_TMPDIR/upper_src" >/dev/null
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/UPPER.PK4" x.txt
    popd >/dev/null
    [ "$status" -eq 0 ]
    sig=$(dd if="$BATS_TEST_TMPDIR/UPPER.PK4" bs=1 count=4 2>/dev/null \
            | od -An -tx1 | tr -d ' \n')
    [ "$sig" = "504b0304" ]
}

@test "pk4 delete + close: rebuild path produces a valid PK4" {
    # Mirrors tests/pk3.bats:343. Exercises the pakka_commit ZIP
    # dispatch arm under a PK4 label — the one bats-only gap codex
    # flagged in the plan review.
    rm -rf "$BATS_TEST_TMPDIR/del_src" "$BATS_TEST_TMPDIR/del_out"
    mkdir -p "$BATS_TEST_TMPDIR/del_src" "$BATS_TEST_TMPDIR/del_out"
    echo "keep" > "$BATS_TEST_TMPDIR/del_src/keep.txt"
    echo "remove" > "$BATS_TEST_TMPDIR/del_src/remove.txt"

    pushd "$BATS_TEST_TMPDIR/del_src" >/dev/null
    "$PAKKA" -cf "$BATS_TEST_TMPDIR/del.pk4" keep.txt remove.txt
    popd >/dev/null

    "$PAKKA" -df "$BATS_TEST_TMPDIR/del.pk4" remove.txt
    [ "$?" -eq 0 ]

    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/del.pk4"
    [ "$status" -eq 0 ]

    "$PAKKA" -xf "$BATS_TEST_TMPDIR/del.pk4" -C "$BATS_TEST_TMPDIR/del_out"
    [ -f "$BATS_TEST_TMPDIR/del_out/keep.txt" ]
    [ ! -f "$BATS_TEST_TMPDIR/del_out/remove.txt" ]
}

@test "pk4 open: PACK magic in a .pk4 file opens as PAK (magic wins)" {
    # Extension is only the label tiebreaker between PK3 and PK4 — magic
    # bytes still decide whether the file is a ZIP at all. A PAK file
    # renamed to .pk4 must read as a PAK, not be rejected.
    rm -rf "$BATS_TEST_TMPDIR/pak_src"
    mkdir -p "$BATS_TEST_TMPDIR/pak_src"
    echo "pak content" > "$BATS_TEST_TMPDIR/pak_src/p.txt"

    pushd "$BATS_TEST_TMPDIR/pak_src" >/dev/null
    "$PAKKA" -cf "$BATS_TEST_TMPDIR/real.pak" p.txt
    popd >/dev/null

    cp "$BATS_TEST_TMPDIR/real.pak" "$BATS_TEST_TMPDIR/disguised.pk4"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/disguised.pk4"
    [ "$status" -eq 0 ]
    [[ "$output" == *"p.txt"* ]]
}
