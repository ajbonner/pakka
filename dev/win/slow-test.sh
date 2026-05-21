#!/usr/bin/env bash
#
# Windows / MSYS2 equivalent of `make slow-test` and `make slow-test-goldsrc`.
#
# The Makefile's slow-test targets depend on $(TARGET) (the cc-built `pakka`
# binary) and on `symbol-audit` (which reads libpakka.a). Neither exists in
# the Windows MSVC build path — pakka.exe is produced by CMake/Ninja under
# build/cmake/, and there's no static archive to audit. This script does
# what the Makefile targets do but using the pre-built pakka.exe via the
# PAKKA env var.
#
# The fixture download + SHA verification still runs through `make` because
# `make verify-q3demo` / `make verify-goldsrc-{uplink,dayone}` only use
# curl + openssl, neither of which needs the cc toolchain.
#
# Usage (from MSYS2 MSYS shell, at repo root):
#   dev/win/slow-test.sh            # both suites
#   dev/win/slow-test.sh q3         # Q3 demo PK3 only
#   dev/win/slow-test.sh goldsrc    # Half-Life PAK fixtures only
#
# Override PAKKA if your build lives elsewhere:
#   PAKKA=/c/path/to/pakka.exe dev/win/slow-test.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

PAKKA="${PAKKA:-$REPO_ROOT/build/cmake/pakka.exe}"
if [ ! -x "$PAKKA" ]; then
    echo "slow-test: pakka.exe not found at $PAKKA" >&2
    echo "slow-test: build it first via 'cmake -B build/cmake -G Ninja . && cmake --build build/cmake'" >&2
    echo "slow-test: or override with PAKKA=/path/to/pakka.exe" >&2
    exit 1
fi
export PAKKA

TEST_DIR="$REPO_ROOT/build/test"

run_q3() {
    local zip="$TEST_DIR/q3demo.zip"
    local pk3="$TEST_DIR/q3demo/pak0.pk3"

    make verify-q3demo

    if [ ! -f "$pk3" ]; then
        mkdir -p "$TEST_DIR/q3demo_raw" "$TEST_DIR/q3demo"
        "$PAKKA" -x -C "$TEST_DIR/q3demo_raw" "$zip" >/dev/null
        cp "$TEST_DIR/q3demo_raw/Quake 3 Arena Demo/demoq3/pak0.pk3" "$pk3"
        echo "==> Q3 demo pak0.pk3 ready: $pk3"
    fi

    Q3DEMO_PAK0_PK3="$pk3" bats tests/pk3_q3demo.bats
}

run_goldsrc() {
    local up_zip="$TEST_DIR/hl-uplink.zip"
    local up_pak="$TEST_DIR/hl-uplink/valve/pak0.pak"
    local do_zip="$TEST_DIR/hl-dayone.zip"
    local do_pak="$TEST_DIR/hl-dayone/valve/pak0.pak"

    make verify-goldsrc-uplink verify-goldsrc-dayone

    if [ ! -f "$up_pak" ]; then
        mkdir -p "$TEST_DIR/hl-uplink-raw" "$TEST_DIR/hl-uplink/valve"
        unzip -q -o -d "$TEST_DIR/hl-uplink-raw" "$up_zip"
        cp "$TEST_DIR/hl-uplink-raw/Half-LifeUplink/valve/pak0.PAK" "$up_pak"
        echo "==> Half-Life Uplink pak0.pak ready: $up_pak"
    fi

    if [ ! -f "$do_pak" ]; then
        mkdir -p "$TEST_DIR/hl-dayone-raw" "$TEST_DIR/hl-dayone/valve"
        unzip -q -o -d "$TEST_DIR/hl-dayone-raw" "$do_zip"
        cp "$TEST_DIR/hl-dayone-raw/Half-Life Day One/valve/pak0.pak" "$do_pak"
        echo "==> Half-Life: Day One pak0.pak ready: $do_pak"
    fi

    GOLDSRC_UPLINK_PAK0="$up_pak" \
    GOLDSRC_DAYONE_PAK0="$do_pak" \
    bats tests/pak_goldsrc.bats
}

case "${1:-all}" in
    q3)       run_q3 ;;
    goldsrc)  run_goldsrc ;;
    all)      run_q3; run_goldsrc ;;
    *)
        echo "usage: $0 [q3|goldsrc|all]" >&2
        exit 2
        ;;
esac
