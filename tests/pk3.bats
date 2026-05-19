#!/usr/bin/env bats
#
# PK3 / ZIP container support tests. Synthetic fixtures only — each
# fixture is built fresh from a directory tree using /usr/bin/zip
# (already required by setup_file). No external download.

PROJECT_ROOT="${BATS_TEST_DIRNAME}/.."
PAKKA="${PAKKA:-${PROJECT_ROOT}/pakka}"

# Build a small PK3 with mixed STORED + DEFLATE entries.
setup_file() {
    mkdir -p "$BATS_FILE_TMPDIR/src/sub"
    echo -n "hello pk3" > "$BATS_FILE_TMPDIR/src/hello.txt"
    printf 'binary\x00\x01\x02\x03data' > "$BATS_FILE_TMPDIR/src/binary.bin"
    echo "nested" > "$BATS_FILE_TMPDIR/src/sub/nested.txt"
    # Build a compressible payload large enough for zip to choose DEFLATE.
    python3 -c "open('$BATS_FILE_TMPDIR/src/lorem.txt','w').write(('The quick brown fox jumps over the lazy dog. ' * 250))"

    (cd "$BATS_FILE_TMPDIR/src" && zip -q9 "$BATS_FILE_TMPDIR/mixed.pk3" -r .)
}

@test "pk3 list: enumerates entries" {
    run "$PAKKA" -lf "$BATS_FILE_TMPDIR/mixed.pk3"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello.txt"* ]]
    [[ "$output" == *"sub/nested.txt"* ]]
    [[ "$output" == *"lorem.txt"* ]]
}

@test "pk3 list --tree: renders hierarchy" {
    run "$PAKKA" -lf --tree "$BATS_FILE_TMPDIR/mixed.pk3"
    [ "$status" -eq 0 ]
    cleaned="${output//$'\r'/}"
    [[ "$cleaned" == *"sub"* ]]
    [[ "$cleaned" == *"nested.txt"* ]]
}

@test "pk3 extract: STORED + DEFLATE entries match source bytes" {
    rm -rf "$BATS_TEST_TMPDIR/out"
    mkdir -p "$BATS_TEST_TMPDIR/out"
    run "$PAKKA" -xf "$BATS_FILE_TMPDIR/mixed.pk3" -C "$BATS_TEST_TMPDIR/out"
    [ "$status" -eq 0 ]
    diff -r "$BATS_FILE_TMPDIR/src" "$BATS_TEST_TMPDIR/out"
}

@test "pk3 extract: selective by entry name" {
    rm -rf "$BATS_TEST_TMPDIR/sel"
    mkdir -p "$BATS_TEST_TMPDIR/sel"
    run "$PAKKA" -xf "$BATS_FILE_TMPDIR/mixed.pk3" -C "$BATS_TEST_TMPDIR/sel" hello.txt
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/sel/hello.txt" ]
    # No other entries extracted
    [ ! -f "$BATS_TEST_TMPDIR/sel/binary.bin" ]
    [ ! -f "$BATS_TEST_TMPDIR/sel/sub/nested.txt" ]
}

@test "pk3 create: -c .pk3 builds a STORED PK3 that round-trips" {
    rm -rf "$BATS_TEST_TMPDIR/build_src" "$BATS_TEST_TMPDIR/build_out"
    mkdir -p "$BATS_TEST_TMPDIR/build_src/d" "$BATS_TEST_TMPDIR/build_out"
    echo "a" > "$BATS_TEST_TMPDIR/build_src/a.txt"
    echo "b nested" > "$BATS_TEST_TMPDIR/build_src/d/b.txt"

    pushd "$BATS_TEST_TMPDIR/build_src" >/dev/null
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/built.pk3" a.txt d
    popd >/dev/null
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/built.pk3" ]

    # First 4 bytes must be PK\003\004 (LFH) since we wrote entries.
    # `dd bs=1 count=N` is POSIX; OpenBSD's `head` has no -c, and BSDs
    # don't ship xxd by default.
    sig=$(dd if="$BATS_TEST_TMPDIR/built.pk3" bs=1 count=4 2>/dev/null \
            | od -An -tx1 | tr -d ' \n')
    [ "$sig" = "504b0304" ]

    # unzip understands our output — independent reader check.
    run unzip -t "$BATS_TEST_TMPDIR/built.pk3"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No errors detected"* ]]

    # Pakka extracts what pakka built.
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/built.pk3" -C "$BATS_TEST_TMPDIR/build_out"
    [ "$status" -eq 0 ]
    diff -r "$BATS_TEST_TMPDIR/build_src" "$BATS_TEST_TMPDIR/build_out"
}

@test "pk3 create: empty .pk3 is a 22-byte EOCD-only archive" {
    run "$PAKKA" -cf "$BATS_TEST_TMPDIR/empty.pk3"
    [ "$status" -eq 0 ]
    [ -f "$BATS_TEST_TMPDIR/empty.pk3" ]
    # 22 bytes = PK\005\006 + 18 zero bytes
    size=$(wc -c < "$BATS_TEST_TMPDIR/empty.pk3")
    [ "$size" -eq 22 ]
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/empty.pk3"
    [ "$status" -eq 0 ]
    [[ "$output" == *"empty"* ]] || [ -z "$output" ]
}

@test "pk3 open: rejects multi-disk spanning marker" {
    # PK\007\008 alone is enough; we don't even need a valid archive.
    printf 'PK\007\010' > "$BATS_TEST_TMPDIR/span.pk3"
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/span.pk3"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Multi-disk"* ]] || [[ "$stderr" == *"Multi-disk"* ]]
}

@test "pk3 extract: refuses entry with '..' traversal" {
    # Build a PK3 with an entry whose name traverses upward.
    rm -rf "$BATS_TEST_TMPDIR/evil"
    mkdir -p "$BATS_TEST_TMPDIR/evil"
    echo evil > "$BATS_TEST_TMPDIR/evil_payload"
    # Use zip's -j to drop dirs, then we can't easily inject ..,
    # so build the bytes manually via python.
    python3 - "$BATS_TEST_TMPDIR/traversal.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"../escape.txt"
payload = b"escape\n"
crc = zlib.crc32(payload) & 0xffffffff
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 0, 0, 0x0021, crc, len(payload), len(payload),
    len(name), 0) + name + payload
cdr_offset = len(lfh)
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 0, 0, 0x0021, crc, len(payload), len(payload),
    len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), cdr_offset, 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    rm -rf "$BATS_TEST_TMPDIR/out_evil"
    mkdir -p "$BATS_TEST_TMPDIR/out_evil"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/traversal.pk3" -C "$BATS_TEST_TMPDIR/out_evil"
    [ "$status" -ne 0 ]
    # Confirm nothing escaped
    [ ! -f "$BATS_TEST_TMPDIR/escape.txt" ]
}

@test "pk3 open: rejects entry with Windows reserved device name (CON.txt)" {
    python3 - "$BATS_TEST_TMPDIR/reserved.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"CON.txt"
payload = b"x"
crc = zlib.crc32(payload) & 0xffffffff
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 0, 0, 0x0021, crc, 1, 1, len(name), 0) + name + payload
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 0, 0, 0x0021, crc, 1, 1, len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), len(lfh), 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    rm -rf "$BATS_TEST_TMPDIR/out_res"
    mkdir -p "$BATS_TEST_TMPDIR/out_res"
    run "$PAKKA" -xf "$BATS_TEST_TMPDIR/reserved.pk3" -C "$BATS_TEST_TMPDIR/out_res"
    [ "$status" -ne 0 ]
}

@test "pk3 open: refuses ZIP64 sentinels" {
    # Build an EOCD that claims 0xFFFF total entries — ZIP64 sentinel.
    python3 - "$BATS_TEST_TMPDIR/z64.pk3" <<'PY'
import struct, sys
path = sys.argv[1]
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 0xFFFF, 0xFFFF, 0, 0, 0)
with open(path, "wb") as f:
    f.write(eocd)
PY
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/z64.pk3"
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"ZIP64"* ]] || [[ "$output$stderr" == *"not supported"* ]]
}

@test "pk3 open: refuses encrypted entry (general purpose bit 0)" {
    python3 - "$BATS_TEST_TMPDIR/enc.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"a.txt"
payload = b"x"
crc = zlib.crc32(payload) & 0xffffffff
# flags=0x0001 (encrypted)
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 1, 0, 0, 0x0021, crc, 1, 1, len(name), 0) + name + payload
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 1, 0, 0, 0x0021, crc, 1, 1, len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), len(lfh), 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/enc.pk3"
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"Encrypted"* ]]
}

@test "pk3 open: refuses unsupported compression method (12 = bzip2)" {
    python3 - "$BATS_TEST_TMPDIR/bz.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"a.txt"
payload = b"x"
crc = zlib.crc32(payload) & 0xffffffff
# method=12 (bzip2)
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 12, 0, 0x0021, crc, 1, 1, len(name), 0) + name + payload
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 12, 0, 0x0021, crc, 1, 1, len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), len(lfh), 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/bz.pk3"
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"method"* ]]
}

@test "pk3 format: pakka_format() returns PAKKA_FORMAT_PK3 for opened PK3" {
    command -v ${CC:-cc} >/dev/null 2>&1 || skip "no cc in PATH (MSYS2 / MSVC build)"
    cat > "$BATS_TEST_TMPDIR/fmt_test.c" <<'EOF'
#include <stdio.h>
#include "pakka.h"
int main(int argc, char **argv) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_status_t s = pakka_open(argv[1], PAKKA_OPEN_READ, &a, &err);
    if (s != PAKKA_OK) { fprintf(stderr, "open: %s\n", err.message); return 2; }
    pakka_format_t f = pakka_format(a);
    pakka_close(a, &err);
    if (f != PAKKA_FORMAT_PK3) {
        fprintf(stderr, "expected PAKKA_FORMAT_PK3 (%d), got %d\n",
                (int)PAKKA_FORMAT_PK3, (int)f);
        return 3;
    }
    return 0;
}
EOF
    ${CC:-cc} ${CFLAGS:-} -I"${PROJECT_ROOT}/include" \
        -o "$BATS_TEST_TMPDIR/fmt_test" \
        "$BATS_TEST_TMPDIR/fmt_test.c" \
        "${PROJECT_ROOT}/build/lib/libpakka.a"
    run "$BATS_TEST_TMPDIR/fmt_test" "$BATS_FILE_TMPDIR/mixed.pk3"
    [ "$status" -eq 0 ]
}

@test "pk3 deep verify: catches DEFLATE CRC mismatch" {
    # Build a PK3 with a deliberately wrong CRC for a DEFLATE entry.
    python3 - "$BATS_TEST_TMPDIR/bad_crc.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"a.txt"
payload = b"hello world, this is a payload that compresses"
# DEFLATE the payload (raw, no zlib wrapper)
co = zlib.compressobj(level=9, wbits=-15)
compressed = co.compress(payload) + co.flush()
real_crc = zlib.crc32(payload) & 0xffffffff
bad_crc = real_crc ^ 0xFFFFFFFF  # flip every bit
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 8, 0, 0x0021, bad_crc, len(compressed), len(payload),
    len(name), 0) + name + compressed
cdr_offset = len(lfh)
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 8, 0, 0x0021, bad_crc, len(compressed), len(payload),
    len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), cdr_offset, 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    # Structural verify must pass (CRC isn't checked).
    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/bad_crc.pk3"
    [ "$status" -eq 0 ]
    # Deep verify must fail.
    run "$PAKKA" --verify --deep -f "$BATS_TEST_TMPDIR/bad_crc.pk3"
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"CRC32 mismatch"* ]]
}

@test "pk3 open: refuses STORED entry with csize != usize" {
    python3 - "$BATS_TEST_TMPDIR/stored_mismatch.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"a.txt"
payload = b"x" * 10
crc = zlib.crc32(payload) & 0xffffffff
# method=0 (STORED), csize=10, usize=5 — mismatch.
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 0, 0, 0x0021, crc, 10, 5, len(name), 0) + name + payload
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 0, 0, 0x0021, crc, 10, 5, len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), len(lfh), 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/stored_mismatch.pk3"
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"csize != usize"* ]]
}

@test "pk3 open: refuses LFH whose payload overlaps the CDR" {
    # CDR offset is correct, but the LFH claims a compressed_size that
    # would push payload bytes into the CDR region.
    python3 - "$BATS_TEST_TMPDIR/overlap.pk3" <<'PY'
import struct, sys, zlib
path = sys.argv[1]
name = b"a.txt"
payload = b"x"
crc = zlib.crc32(payload) & 0xffffffff
# LFH claims csize=100 but only 1 real byte exists before CDR.
lfh = b"PK\x03\x04" + struct.pack("<HHHHHIIIHH",
    20, 0, 0, 0, 0x0021, crc, 100, 100, len(name), 0) + name + payload
cdr_offset = len(lfh)
cdr = b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII",
    20, 20, 0, 0, 0, 0x0021, crc, 100, 100, len(name), 0, 0, 0, 0, 0, 0) + name
eocd = b"PK\x05\x06" + struct.pack("<HHHHIIH",
    0, 0, 1, 1, len(cdr), cdr_offset, 0)
with open(path, "wb") as f:
    f.write(lfh + cdr + eocd)
PY
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/overlap.pk3"
    [ "$status" -ne 0 ]
    # Pakka should reject either with csize != usize (caught earlier),
    # CDR-overlap, or LFH-disagrees-with-CDR. All are correct refusals.
}

@test "pk3 add: refuses duplicate entry name" {
    rm -f "$BATS_TEST_TMPDIR/dup.pk3"
    cd "$BATS_TEST_TMPDIR"
    echo "first" > a.txt
    "$PAKKA" -cf dup.pk3 a.txt
    [ -f dup.pk3 ]
    # Try to add the same entry again.
    echo "second" > a.txt
    run "$PAKKA" -af dup.pk3 a.txt
    [ "$status" -ne 0 ]
    [[ "$output$stderr" == *"duplicate"* ]] || [[ "$output$stderr" == *"Duplicate"* ]]
}

@test "pk3 delete + close: rebuild path produces valid PK3" {
    rm -rf "$BATS_TEST_TMPDIR/del_src" "$BATS_TEST_TMPDIR/del_out"
    mkdir -p "$BATS_TEST_TMPDIR/del_src" "$BATS_TEST_TMPDIR/del_out"
    echo "keep" > "$BATS_TEST_TMPDIR/del_src/keep.txt"
    echo "remove" > "$BATS_TEST_TMPDIR/del_src/remove.txt"

    pushd "$BATS_TEST_TMPDIR/del_src" >/dev/null
    "$PAKKA" -cf "$BATS_TEST_TMPDIR/del.pk3" keep.txt remove.txt
    popd >/dev/null

    "$PAKKA" -df "$BATS_TEST_TMPDIR/del.pk3" remove.txt
    [ "$?" -eq 0 ]

    # pakka's own --verify confirms structural integrity post-delete.
    # (Avoids depending on `unzip`, which isn't in every BSD base.)
    run "$PAKKA" --verify -f "$BATS_TEST_TMPDIR/del.pk3"
    [ "$status" -eq 0 ]

    "$PAKKA" -xf "$BATS_TEST_TMPDIR/del.pk3" -C "$BATS_TEST_TMPDIR/del_out"
    [ -f "$BATS_TEST_TMPDIR/del_out/keep.txt" ]
    [ ! -f "$BATS_TEST_TMPDIR/del_out/remove.txt" ]
}

@test "pk3 c-api: max_decompressed_size cap refuses oversize entries" {
    command -v ${CC:-cc} >/dev/null 2>&1 || skip "no cc in PATH (MSYS2 / MSVC build)"
    # The default cap is 64 MiB; we exercise the cap from C with the
    # cap dropped to 100 bytes against an 11 KB DEFLATE entry, which
    # must be refused with PAKKA_ERR_LIMIT.
    cat > "$BATS_TEST_TMPDIR/cap_test.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "pakka.h"
int main(int argc, char **argv) {
    pakka_archive_t *a = NULL;
    pakka_error_t err;
    pakka_reader_t *r = NULL;
    pakka_status_t s = pakka_open(argv[1], PAKKA_OPEN_READ, &a, &err);
    if (s != PAKKA_OK) { fprintf(stderr, "open failed: %s\n", err.message); return 2; }
    s = pakka_set_max_decompressed_size(a, 100, &err);
    if (s != PAKKA_OK) { fprintf(stderr, "set cap failed\n"); return 3; }
    s = pakka_open_entry(a, "lorem.txt", &r, &err);
    if (s != PAKKA_ERR_LIMIT) {
        fprintf(stderr, "expected PAKKA_ERR_LIMIT (9), got %d: %s\n", (int)s, err.message);
        return 4;
    }
    if (r != NULL) { fprintf(stderr, "reader must be NULL on cap failure\n"); return 5; }
    pakka_close(a, &err);
    return 0;
}
EOF
    ${CC:-cc} ${CFLAGS:-} -I"${PROJECT_ROOT}/include" \
        -o "$BATS_TEST_TMPDIR/cap_test" \
        "$BATS_TEST_TMPDIR/cap_test.c" \
        "${PROJECT_ROOT}/build/lib/libpakka.a"
    run "$BATS_TEST_TMPDIR/cap_test" "$BATS_FILE_TMPDIR/mixed.pk3"
    [ "$status" -eq 0 ]
}

@test "pk3 open: directory entries (trailing /) are silently skipped" {
    # zip -r includes empty subdirs as size-zero entries named "foo/"
    mkdir -p "$BATS_TEST_TMPDIR/dirsrc/empty_dir"
    echo "real" > "$BATS_TEST_TMPDIR/dirsrc/real.txt"
    (cd "$BATS_TEST_TMPDIR/dirsrc" && zip -q "$BATS_TEST_TMPDIR/dirs.pk3" -r .)

    # CDR claims 2 entries (real.txt + empty_dir/); pakka should expose only 1.
    run "$PAKKA" -lf "$BATS_TEST_TMPDIR/dirs.pk3"
    [ "$status" -eq 0 ]
    [[ "$output" == *"real.txt"* ]]
    # No "empty_dir/" in listing
    [[ "$output" != *"empty_dir/"* ]]
}
