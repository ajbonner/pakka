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
# PAKKA can be pre-set by the caller (Windows CI points it at the MSVC
# build's pakka.exe; the Makefile-driven path leaves it unset and we
# fall through to the default location).
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"
PAK0="${PROJECT_ROOT}/build/test/pak0.pak"

# Build a minimal valid pak with a single directory entry of the given
# name and a zero-length payload. Used by the path-traversal tests to
# inject names that would never come from pakka's own pack path.
# Layout: 12-byte header + 64-byte dir entry, no payload bytes.
write_pak_one_entry() {
    # NB: split `local` over two statements — `local a="$1" b="$2" c=${#b}`
    # evaluates `${#b}` *before* b="$2" is processed, yielding zero.
    local pakfile="$1" name="$2"
    local pad=$((56 - ${#name}))
    {
        # PACK + diroffset=12 (LE) + dirlength=64 (LE)
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf '%s' "$name"
        dd if=/dev/zero bs=1 count="$pad" 2>/dev/null
        # offset=76 (past directory; never read because we error first)
        # length=0
        printf '\114\000\000\000\000\000\000\000'
    } > "$pakfile"
}

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
    # Pak format constants — must match src/common.h.
    local PAK_HEADER_SIZE=12
    local PAK_SIG_LEN=4

    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"

    all_paths=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | awk '{print $1}')
    # shellcheck disable=SC2086  # intentional word-splitting for the path list
    "$PAKKA" -df "$BATS_TEST_TMPDIR/work.pak" $all_paths >/dev/null

    [ "$(wc -c < "$BATS_TEST_TMPDIR/work.pak" | tr -d ' ')" -eq "$PAK_HEADER_SIZE" ]
    # OpenBSD head(1) has no -c; dd is the portable byte-count read.
    [ "$(dd if="$BATS_TEST_TMPDIR/work.pak" bs=1 count="$PAK_SIG_LEN" 2>/dev/null)" = "PACK" ]

    # diroffset (LE u32) sits immediately after the signature and must point
    # to the end of the (empty) header. dirlength == 0 is implied by the
    # file being exactly header-sized (and confirmed via -lf below).
    # Read four bytes as hex and reassemble LE -> decimal in bash so the
    # check is correct regardless of host endianness (s390x) and is
    # robust across GNU/BSD/busybox `od` formatting differences (`-tu1`
    # column widths vary between OpenBSD and GNU; `-tx1` is uniform).
    hex=$(od -An -tx1 -N "$PAK_SIG_LEN" -j "$PAK_SIG_LEN" "$BATS_TEST_TMPDIR/work.pak" | tr -d ' \n')
    diroffset=$(( 16#${hex:0:2} + (16#${hex:2:2} << 8) + (16#${hex:4:2} << 16) + (16#${hex:6:2} << 24) ))
    [ "$diroffset" -eq "$PAK_HEADER_SIZE" ]

    # Listing should succeed and report empty rather than crashing.
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Pak is empty"

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

@test "list --tree: renders hierarchy with sorted branches and summary" {
    mkdir -p "$BATS_TEST_TMPDIR/src/gfx" "$BATS_TEST_TMPDIR/src/maps"
    printf 'cfg\n'    > "$BATS_TEST_TMPDIR/src/default.cfg"
    printf 'dot1\n'   > "$BATS_TEST_TMPDIR/src/gfx/menudot1.lmp"
    printf 'dot2\n'   > "$BATS_TEST_TMPDIR/src/gfx/menudot2.lmp"
    printf 'pop\n'    > "$BATS_TEST_TMPDIR/src/gfx/pop.lmp"
    printf 'battle\n' > "$BATS_TEST_TMPDIR/src/maps/b_batt0.bsp"
    printf 'start\n'  > "$BATS_TEST_TMPDIR/src/maps/start.bsp"
    printf 'quake\n'  > "$BATS_TEST_TMPDIR/src/quake.rc"

    (cd "$BATS_TEST_TMPDIR/src" && "$PAKKA" -cf "$BATS_TEST_TMPDIR/tree.pak" default.cfg gfx maps quake.rc) >/dev/null

    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/tree.pak" --tree
    [ "$status" -eq 0 ]

    expected=$(cat <<'EOF'
.
├── default.cfg
├── gfx
│   ├── menudot1.lmp
│   ├── menudot2.lmp
│   └── pop.lmp
├── maps
│   ├── b_batt0.bsp
│   └── start.bsp
└── quake.rc

2 directories, 7 files
EOF
)

    # Strip \r so MSVC-built pakka.exe (whose stdout is text-mode by
    # default and emits CRLF) matches the same expected text as POSIX.
    [ "${output//$'\r'/}" = "$expected" ]
}

@test "list --tree: empty pak prints root and zero summary" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    all_paths=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | awk '{print $1}')
    # shellcheck disable=SC2086  # intentional word-splitting for the path list
    "$PAKKA" -df "$BATS_TEST_TMPDIR/work.pak" $all_paths >/dev/null

    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" --tree
    [ "$status" -eq 0 ]

    expected=$(printf '.\n\n0 directories, 0 files')
    [ "${output//$'\r'/}" = "$expected" ]
}

@test "list --tree: rejected when combined with a non-list mode" {
    run "$PAKKA" -xf "$PAK0" --tree -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -- "--tree may only be used with -l"
}

@test "list --tree: literal --tree path is preserved after --" {
    # Sanity: "--" should end option parsing so a path literally named
    # "--tree" can still be matched as a pak entry.
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/out" -- --tree
    # --tree is not in the pak; we expect a not-found error, NOT the
    # "--tree may only be used with -l" message that fires when --tree
    # is consumed as an option.
    [ "$status" -ne 0 ]
    ! echo "$output" | grep -q -- "--tree may only be used with -l"
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

# Per-name policy (Windows reserved devices, ADS colon, trailing dot/space,
# control bytes): paks are portable, so a name that would resolve to a
# device or get silently normalized on Windows must be rejected on every
# host even if a Linux extract would technically work. write_pak_one_entry
# rejects names whose length needs padding < 0, which limits these to 56
# bytes — well within real-world payload names.

@test "extract: refuses Windows reserved device name (CON)" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'CON'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses Windows reserved device name with extension (NUL.txt)" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'NUL.txt'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses reserved device in subdirectory (foo/COM1)" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'foo/COM1'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses ADS colon in name (file:stream)" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'file:stream'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses trailing dot on segment (foo.)" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'foo.'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses trailing space on segment (foo )" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'foo '
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses control byte in name" {
    # printf'd literal ESC (0x1b) inside the entry name.
    name=$(printf 'esc\033inj')
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" "$name"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: COM10 is allowed (only COM0-9 are reserved)" {
    # Build a pak with one zero-length entry named "COM10". Should
    # extract cleanly since "COM10" is not a reserved Windows device.
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf 'COM10'
        dd if=/dev/zero bs=1 count=$((56 - 5)) 2>/dev/null
        printf '\114\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/safe.pak"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/safe.pak" -C "$BATS_TEST_TMPDIR/out" >/dev/null
    [ -f "$BATS_TEST_TMPDIR/out/COM10" ]
}

# H2 — symlink-safe extract. compat_open_extract_target walks pak entry
# components with O_NOFOLLOW (POSIX) or reparse-point checks (Windows),
# so a pre-planted symlink in the -C destination cannot redirect a write.

@test "extract: refuses to follow symlink in destination tree (POSIX)" {
    # Skip on Windows: symlinks need elevated privileges and the bats
    # job runs unprivileged. PAKKA being .exe is the simplest tell.
    case "$PAKKA" in *.exe) skip "POSIX-only test" ;; esac

    # Build a pak with one zero-length entry "models/x" — safe name.
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf 'models/x'
        dd if=/dev/zero bs=1 count=$((56 - 8)) 2>/dev/null
        printf '\114\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/ok.pak"

    mkdir -p "$BATS_TEST_TMPDIR/out" "$BATS_TEST_TMPDIR/outside"
    ln -s "$BATS_TEST_TMPDIR/outside" "$BATS_TEST_TMPDIR/out/models"

    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/ok.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    # The escape target must remain untouched.
    [ ! -e "$BATS_TEST_TMPDIR/outside/x" ]
}

# H3 — preflight extract: a safe entry preceding a traversal entry must
# not leave the safe entry on disk when the traversal causes the extract
# to error.

@test "extract: refuses pak where entries collide after normalization (case fold)" {
    # Two entries differ only in case. On Windows/HFS+ they materialize
    # to the same file; pakka rejects the whole extraction at preflight
    # so we never get a half-extracted output tree.
    #
    # Layout (LE):
    #   bytes  0..11 : "PACK" + diroffset=12 + dirlength=128
    #   bytes 12..75 : dir entry 0 = "Foo.txt" (offset=140, length=0)
    #   bytes 76..139: dir entry 1 = "foo.txt" (offset=140, length=0)
    {
        printf 'PACK\014\000\000\000\200\000\000\000'
        printf 'Foo.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        printf 'foo.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/case_collide.pak"

    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/case_collide.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    [[ "$output" == *"collide after normalization"* ]] \
        || [[ "${stderr:-$output}" == *"collide after normalization"* ]]
    # Neither variant should have been materialized.
    [ ! -e "$BATS_TEST_TMPDIR/out/Foo.txt" ]
    [ ! -e "$BATS_TEST_TMPDIR/out/foo.txt" ]
}

@test "extract: refuses pak where entries collide after normalization (slash vs backslash)" {
    # "dir/file" and "dir\file" normalize to the same path. Same
    # preflight rejection as the case-fold variant.
    {
        printf 'PACK\014\000\000\000\200\000\000\000'
        printf 'dir/file'
        dd if=/dev/zero bs=1 count=$((56 - 8)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        printf 'dir\\file'
        dd if=/dev/zero bs=1 count=$((56 - 8)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/sep_collide.pak"

    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/sep_collide.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    [[ "$output" == *"collide after normalization"* ]] \
        || [[ "${stderr:-$output}" == *"collide after normalization"* ]]
}

@test "extract: preflight rejects entire archive on later unsafe entry" {
    # Two entries: "safe/a" (legit), then "../escape" (rejected). Both
    # have zero length. Pre-fix, "safe/a" was written before the second
    # entry's validation fired.
    #
    # Layout (LE):
    #   bytes  0..11 : "PACK" + diroffset=12 + dirlength=128
    #   bytes 12..75 : dir entry 0 = "safe/a" (offset=140, length=0)
    #   bytes 76..139: dir entry 1 = "../escape" (offset=140, length=0)
    {
        printf 'PACK\014\000\000\000\200\000\000\000'
        printf 'safe/a'
        dd if=/dev/zero bs=1 count=$((56 - 6)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        printf '../escape'
        dd if=/dev/zero bs=1 count=$((56 - 9)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/mixed.pak"

    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/mixed.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    # The earlier "safe/a" must not have been materialized.
    [ ! -e "$BATS_TEST_TMPDIR/out/safe/a" ]
    [ ! -d "$BATS_TEST_TMPDIR/out/safe" ]
}

# H6 — recursive add must not silently follow a symlink into a tree
# the user didn't ask us to pack.

@test "add: skips symlinks inside a directory tree" {
    case "$PAKKA" in *.exe) skip "POSIX-only test" ;; esac

    mkdir -p "$BATS_TEST_TMPDIR/src/sub" "$BATS_TEST_TMPDIR/elsewhere"
    printf 'real\n' > "$BATS_TEST_TMPDIR/src/sub/real.txt"
    printf 'OFF-LIMITS\n' > "$BATS_TEST_TMPDIR/elsewhere/secret.txt"
    ln -s "$BATS_TEST_TMPDIR/elsewhere/secret.txt" "$BATS_TEST_TMPDIR/src/sub/leak"

    (cd "$BATS_TEST_TMPDIR" && "$PAKKA" -cf out.pak src) >/dev/null

    listing=$("$PAKKA" -lf "$BATS_TEST_TMPDIR/out.pak")
    echo "$listing" | grep -q 'src/sub/real\.txt'
    # The symlink must not be in the pak.
    ! echo "$listing" | grep -q 'leak'
    ! echo "$listing" | grep -q 'secret'
}

@test "add: rejects symlink as top-level path argument" {
    case "$PAKKA" in *.exe) skip "POSIX-only test" ;; esac

    printf 'real\n' > "$BATS_TEST_TMPDIR/real.txt"
    ln -s "$BATS_TEST_TMPDIR/real.txt" "$BATS_TEST_TMPDIR/link.txt"

    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    run "$PAKKA" -af "$BATS_TEST_TMPDIR/work.pak" "$BATS_TEST_TMPDIR/link.txt"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "symlink"
}

# H12 — duplicate name rejection on add.

@test "add: rejects duplicate name within the same pak" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    printf 'first\n' > "$BATS_TEST_TMPDIR/dupe.txt"
    (cd "$BATS_TEST_TMPDIR" && "$PAKKA" -af work.pak dupe.txt) >/dev/null

    # Same name, second time — must refuse.
    printf 'second\n' > "$BATS_TEST_TMPDIR/dupe.txt"
    run bash -c "cd '$BATS_TEST_TMPDIR' && '$PAKKA' -af work.pak dupe.txt"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "already exists"
}

# H5 — create with no-replace semantics. If the destination appears
# after create_pakfile's stat() check, close_pakfile must refuse to
# clobber it rather than silently overwriting.

@test "create: refuses to overwrite destination that appeared during the run" {
    # Simulate the race by creating the destination AFTER pakka has
    # opened the temp pak. We can't pause pakka mid-run from bats, but
    # the no-replace rename catches the case where the dest exists at
    # close time. Approximation: pre-create the destination — pakka's
    # stat-at-start check should also fire, but turning that off
    # would mean editing the binary. Instead, validate that an
    # existing destination causes a refusal (the same code path that
    # fires in the race window).
    printf 'do not clobber me\n' > "$BATS_TEST_TMPDIR/target.pak"
    printf 'payload\n' > "$BATS_TEST_TMPDIR/payload"
    (cd "$BATS_TEST_TMPDIR" && run "$PAKKA" -cf target.pak payload) || true
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/target.pak" "$BATS_TEST_TMPDIR/payload"
    [ "$status" -ne 0 ]
    # Original content survived.
    [ "$(cat "$BATS_TEST_TMPDIR/target.pak")" = "do not clobber me" ]
}

# Wave 3 — CLI arg validation, -C sanity, read-only opens, create-time
# entry-name validation, terminal-injection defense in depth.

@test "cli: rejects -d with no path arguments" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    run "$PAKKA" -df "$BATS_TEST_TMPDIR/work.pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "requires at least one path"
}

@test "cli: rejects -a with no path arguments" {
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    run "$PAKKA" -af "$BATS_TEST_TMPDIR/work.pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "requires at least one path"
}

@test "cli: rejects empty -f pakfile name" {
    run "$PAKKA" -lf ""
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "pakfile name"
}

@test "extract: refuses -C target that is a regular file" {
    printf 'hi\n' > "$BATS_TEST_TMPDIR/not-a-dir"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$PAK0" -C "$BATS_TEST_TMPDIR/not-a-dir"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "not a directory"
}

@test "add: refuses to bake unsafe entry name into pak" {
    # Windows can't create a file literally named "CON" — the OS
    # intercepts the open as a device reference and the printf below
    # never produces a real file. Skip there; the POSIX run still
    # exercises the create-time is_unsafe_extract_path check.
    case "$PAKKA" in *.exe) skip "Windows reserves CON at the OS level" ;; esac

    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    printf 'evil\n' > "$BATS_TEST_TMPDIR/CON"
    run bash -c "cd '$BATS_TEST_TMPDIR' && '$PAKKA' -af work.pak CON"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "not safe to extract"
}

@test "list: read-only pak (chmod -w) still lists" {
    case "$PAKKA" in *.exe) skip "POSIX-only test" ;; esac
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/ro.pak"
    chmod a-w "$BATS_TEST_TMPDIR/ro.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/ro.pak"
    [ "$status" -eq 0 ]
    # Restore so bats cleanup can rm it.
    chmod u+w "$BATS_TEST_TMPDIR/ro.pak"
}

@test "list: read-only pak still extracts" {
    case "$PAKKA" in *.exe) skip "POSIX-only test" ;; esac
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/ro.pak"
    chmod a-w "$BATS_TEST_TMPDIR/ro.pak"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/ro.pak" -C "$BATS_TEST_TMPDIR/out" default.cfg >/dev/null
    [ -f "$BATS_TEST_TMPDIR/out/default.cfg" ]
    chmod u+w "$BATS_TEST_TMPDIR/ro.pak"
}

@test "list: control-byte entry name prints sanitized as '?'" {
    # Build a pak whose entry name contains an ESC byte. is_unsafe_extract_path
    # blocks extract, but list still has to print the name — and must not
    # let the raw byte through to the terminal.
    name=$(printf 'naughty\033name')
    write_pak_one_entry "$BATS_TEST_TMPDIR/inj.pak" "$name"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/inj.pak"
    [ "$status" -eq 0 ]
    # Raw ESC must not appear in output; '?' must.
    ! printf '%s' "$output" | grep -q -- $'\033'
    printf '%s' "$output" | grep -q 'naughty?name'
}

# Path-traversal hardening: a crafted pak can name an entry such that extract
# would write outside the -C destination. Pak entry names are 56 bytes of
# attacker-controlled data; pakka must reject traversal attempts on both
# POSIX and Windows shapes (drive letters, UNC, mixed separators) since
# paks are cross-platform.

@test "extract: refuses '..' path traversal in POSIX-style entry name" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" '../etc/passwd'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
    [ ! -e "$BATS_TEST_TMPDIR/etc" ]
}

@test "extract: refuses absolute POSIX path in entry name" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" '/etc/passwd'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses '..' traversal using backslash separator" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" '..\windows\foo'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses leading-backslash absolute path" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" '\windows\foo'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses Windows drive-letter prefix" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" 'C:\windows\foo'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses UNC \\\\server\\share path" {
    write_pak_one_entry "$BATS_TEST_TMPDIR/evil.pak" '\\server\share\foo'
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "extract: refuses empty (all-NUL) entry name" {
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        dd if=/dev/zero bs=1 count=56 2>/dev/null
        printf '\114\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/evil.pak"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/evil.pak" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "Refusing to extract"
}

@test "add: --as stores source under requested virtual entry name" {
    echo "hello" > "$BATS_TEST_TMPDIR/src.bin"
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/aliased.pak" \
        --as "renamed/hi.txt" "$BATS_TEST_TMPDIR/src.bin"
    [ "$status" -eq 0 ]

    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/aliased.pak"
    [ "$status" -eq 0 ]
    [[ "$output" == *"renamed/hi.txt"* ]]
    # The source file's name (src.bin) must NOT be the entry name.
    [[ "$output" != *"src.bin"* ]]
}

@test "add: --as supports multiple aliased pairs in one invocation" {
    echo "a" > "$BATS_TEST_TMPDIR/a.bin"
    echo "bb" > "$BATS_TEST_TMPDIR/b.bin"
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/multi.pak" \
        --as "virt/a.txt" "$BATS_TEST_TMPDIR/a.bin" \
        --as "virt/b.txt" "$BATS_TEST_TMPDIR/b.bin"
    [ "$status" -eq 0 ]
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/multi.pak"
    [[ "$output" == *"virt/a.txt"* ]]
    [[ "$output" == *"virt/b.txt"* ]]
}

@test "add: --as rejected outside -a/-c" {
    echo "x" > "$BATS_TEST_TMPDIR/x.bin"
    run "$PAKKA" -lf build/test/pak0.pak \
        --as "v/x.txt" "$BATS_TEST_TMPDIR/x.bin"
    [ "$status" -ne 0 ]
    [[ "$output" == *"--as may only be used with -a or -c"* ]] \
        || [[ "${stderr:-$output}" == *"--as may only be used with -a or -c"* ]]
}

@test "add: --as rejects incomplete pair" {
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/inc.pak" --as "virt/only.txt"
    [ "$status" -ne 0 ]
    [[ "$output" == *"--as requires two arguments"* ]] \
        || [[ "${stderr:-$output}" == *"--as requires two arguments"* ]]
}

@test "verify: succeeds on valid pak0.pak" {
    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/../pak0.pak"
    [ "$status" -eq 0 ] || true
    # The exact path under BATS_TEST_TMPDIR may not exist — fall back
    # to the build/test fixture directly.
    if [ "$status" -ne 0 ]; then
        run "$PAKKA" --verify -f build/test/pak0.pak
    fi
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "verify: fails on pak with duplicate exact names" {
    # Same layout as the C-API duplicate test, exercised through the CLI.
    {
        printf 'PACK\014\000\000\000\200\000\000\000'
        printf 'dup.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        printf 'dup.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/dup.pak"

    # pakka_open itself rejects exact duplicates, so --verify never
    # gets to run — the open failure surfaces with PAKKA_ERR_DUPLICATE.
    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/dup.pak"
    [ "$status" -ne 0 ]
    [[ "$output" == *"duplicate"* ]] \
        || [[ "${stderr:-$output}" == *"duplicate"* ]]
}

@test "verify: rejects pak that survives open but has normalized collisions" {
    # Two entries differ only in case: pakka_open accepts these (the
    # exact-duplicate check is byte-equal), but pakka_verify rejects
    # because they collide after portable normalization.
    {
        printf 'PACK\014\000\000\000\200\000\000\000'
        printf 'Foo.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
        printf 'foo.txt'
        dd if=/dev/zero bs=1 count=$((56 - 7)) 2>/dev/null
        printf '\214\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/casecollide.pak"

    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/casecollide.pak"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Normalized collision"* ]] \
        || [[ "${stderr:-$output}" == *"Normalized collision"* ]]
}

@test "open: pak with bad PACK magic is rejected" {
    # Same shape as a valid pak but with the signature flipped. Without
    # the memcmp("PACK") check, this would parse as an empty pak.
    {
        printf 'NOPE\014\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/badmagic.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/badmagic.pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "signature"
}

@test "open: pak with diroffset past EOF is rejected" {
    # Header advertises diroffset=999, dirlength=0 in a 12-byte file.
    # Pre-validator, this loaded as an "empty" pak silently.
    {
        printf 'PACK\347\003\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/bad_diroffset.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/bad_diroffset.pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "directory"
}

@test "open: pak with dirlength wrapping past EOF is rejected" {
    # diroffset=12, dirlength=0xFFFFFFC0 (multiple of 64, but way past EOF).
    # Tests the overflow-safe bounds check.
    {
        printf 'PACK\014\000\000\000\300\377\377\377'
    } > "$BATS_TEST_TMPDIR/bad_dirlength.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/bad_dirlength.pak"
    [ "$status" -ne 0 ]
}

@test "open: pak entry offset+length past EOF is rejected" {
    # Header is valid (dir at 12, length 64), one entry with offset=12
    # but length=999 in a 76-byte file. Without entry bounds validation,
    # extract would malloc & fread bogus length and write garbage.
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf 'bogus'
        dd if=/dev/zero bs=1 count=$((56 - 5)) 2>/dev/null
        printf '\014\000\000\000\347\003\000\000'
    } > "$BATS_TEST_TMPDIR/bad_entry.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/bad_entry.pak"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "out of range"
}

@test "open: pak entry offset inside header is rejected" {
    # Entry offset=4 lands inside the 12-byte header — nonsensical and
    # used historically to obscure entry contents.
    {
        printf 'PACK\014\000\000\000\100\000\000\000'
        printf 'header'
        dd if=/dev/zero bs=1 count=$((56 - 6)) 2>/dev/null
        printf '\004\000\000\000\000\000\000\000'
    } > "$BATS_TEST_TMPDIR/inside_header.pak"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/inside_header.pak"
    [ "$status" -ne 0 ]
}

@test "add: rejects input path that would overflow 56-byte name field" {
    # Pre-fix: strcpy(entry->filename, path) corrupted the heap. Now
    # rejected with a clear error before the copy.
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    long_name=$(printf 'a%.0s' $(seq 1 70))
    echo "x" > "$BATS_TEST_TMPDIR/$long_name"
    run "$PAKKA" -af "$BATS_TEST_TMPDIR/work.pak" "$BATS_TEST_TMPDIR/$long_name"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q -i "too long"
}

@test "add: zero-byte file adds and extracts cleanly" {
    # Pre-fix: `while (fread(bytes, 0, 1, tfd))` infinite-looped on
    # empty files. Now the size==0 branch skips read/write entirely.
    cp "$BATS_FILE_TMPDIR/rebuilt.pak" "$BATS_TEST_TMPDIR/work.pak"
    : > "$BATS_TEST_TMPDIR/empty.txt"
    (cd "$BATS_TEST_TMPDIR" && "$PAKKA" -af work.pak empty.txt) >/dev/null

    "$PAKKA" -lf "$BATS_TEST_TMPDIR/work.pak" | grep -q '^empty\.txt '

    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/out" empty.txt >/dev/null
    [ -f "$BATS_TEST_TMPDIR/out/empty.txt" ]
    [ "$(wc -c < "$BATS_TEST_TMPDIR/out/empty.txt" | tr -d ' ')" -eq 0 ]
}

@test "add: preserves bytes when directory tail < byte tail (non-linear layout)" {
    # Regression for the find_tail bug: id's pak0.pak has entries out of
    # byte order, with the directory's last entry NOT at the file's byte
    # tail. Old code seeked to tail->offset+tail->length and would
    # overwrite live data sitting at higher offsets. The fix walks every
    # entry and seeks to the true max(offset+length).
    #
    # Layout (LE):
    #   bytes   0..11 : "PACK" + diroffset=17 + dirlength=128
    #   bytes  12..16 : "hello" (afile payload)
    #   bytes  17..80 : dir entry 0 = "bfile" (offset=145, length=5)
    #   bytes  81..144: dir entry 1 = "afile" (offset=12,  length=5)
    #   bytes 145..149: "world" (bfile payload)
    #
    # load_directory builds the list head→tail in directory order, so
    # tail = afile (offset+length=17). Pre-fix add_file seeks to byte
    # 17 and overwrites the directory plus bfile's payload at 145.
    {
        printf 'PACK\021\000\000\000\200\000\000\000'
        printf 'hello'
        # dir entry 0: bfile at offset 145
        printf 'bfile'
        dd if=/dev/zero bs=1 count=$((56 - 5)) 2>/dev/null
        printf '\221\000\000\000\005\000\000\000'
        # dir entry 1: afile at offset 12  (this is the dir-order tail)
        printf 'afile'
        dd if=/dev/zero bs=1 count=$((56 - 5)) 2>/dev/null
        printf '\014\000\000\000\005\000\000\000'
        printf 'world'
    } > "$BATS_TEST_TMPDIR/non_linear.pak"

    # Sanity: both entries extract correctly from the original.
    mkdir -p "$BATS_TEST_TMPDIR/pre"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/non_linear.pak" -C "$BATS_TEST_TMPDIR/pre" >/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/pre/afile")" = "hello" ]
    [ "$(cat "$BATS_TEST_TMPDIR/pre/bfile")" = "world" ]

    cp "$BATS_TEST_TMPDIR/non_linear.pak" "$BATS_TEST_TMPDIR/work.pak"
    echo "new content" > "$BATS_TEST_TMPDIR/cfile"
    (cd "$BATS_TEST_TMPDIR" && "$PAKKA" -af work.pak cfile) >/dev/null

    # Both pre-existing payloads must survive; the new one must be there.
    mkdir -p "$BATS_TEST_TMPDIR/post"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/work.pak" -C "$BATS_TEST_TMPDIR/post" >/dev/null
    [ "$(cat "$BATS_TEST_TMPDIR/post/afile")" = "hello" ]
    [ "$(cat "$BATS_TEST_TMPDIR/post/bfile")" = "world" ]
    [ "$(cat "$BATS_TEST_TMPDIR/post/cfile")" = "new content" ]
}

@test "extract: legitimate '..' substring in name (foo..bar) still extracts" {
    # Sanity: only an exact ".." path component is unsafe. Substring
    # patterns like "foo..bar" or "..foo" must continue to work, or
    # we'd break legitimate filenames.
    {
        # Header: diroffset=17 (12-byte header + 5-byte payload), dirlength=64
        printf 'PACK\021\000\000\000\100\000\000\000'
        printf 'hello'
        printf 'foo..bar'
        dd if=/dev/zero bs=1 count=$((56 - 8)) 2>/dev/null
        # offset=12 (start of payload), length=5
        printf '\014\000\000\000\005\000\000\000'
    } > "$BATS_TEST_TMPDIR/legit.pak"

    mkdir -p "$BATS_TEST_TMPDIR/out"
    "$PAKKA" -xf "$BATS_TEST_TMPDIR/legit.pak" -C "$BATS_TEST_TMPDIR/out" >/dev/null
    [ -f "$BATS_TEST_TMPDIR/out/foo..bar" ]
    [ "$(cat "$BATS_TEST_TMPDIR/out/foo..bar")" = "hello" ]
}
