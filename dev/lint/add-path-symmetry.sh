#!/usr/bin/env bash
#
# Advisory lint: detect re-divergence of the add-path source-side
# hardening that H1 fixed. After the M1 preflight hoist, every safety
# check (symlink rejection, lstat / S_ISREG, name validation, duplicate
# detection) lives ABOVE the `if (pakka_format_is_zip(...))` dispatch
# in pakka_add_file / pakka_add_memory. If a future change re-introduces
# any of these inside one branch but not the other, the ZIP and PAK
# paths drift apart again — which is the exact class of bug that
# allowed a `.pk3` --as symlink-follow before the original fix.
#
# This lint is ADVISORY (exits 0 with a warning) by default so it
# doesn't break the build. Run from `make lint-advisory` or directly.
# Set STRICT=1 to make it fail with non-zero on any finding.

set -euo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
SRC="$ROOT/src/pakfile.c"

if [ ! -f "$SRC" ]; then
    echo "advisory-lint: cannot find $SRC" >&2
    exit 2
fi

# Symbols that must NOT appear inside the per-format dispatch branches
# of pakka_add_file / pakka_add_memory. Each was the subject of an
# original-then-drifted check that H1+M1 hoisted.
GUARD_SYMBOLS=(
    "pakka_platform_is_reparse_or_symlink"
    "pakka_platform_lstat"
    "pakka_unsafe_entry_name"
    "find_entry"
)

# Extract the body of a function by name. Returns lines between the
# opening { and the matching closing }. Awk-based so it works on the
# default macOS / Linux toolchain without depending on tree-sitter.
function_body() {
    local fn="$1"
    awk -v fn="$fn" '
        $0 ~ "^"fn"\\(" || $0 ~ "^pakka_status_t " fn "\\(" {
            in_decl = 1
        }
        in_decl && /\{/ {
            depth++
            in_decl = 0
            in_body = 1
            sub(/.*\{/, "")
        }
        in_body {
            for (i = 1; i <= length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") depth++
                else if (c == "}") {
                    depth--
                    if (depth == 0) {
                        in_body = 0
                        exit
                    }
                }
            }
            print
        }
    ' "$SRC"
}

# Extract just the lines inside an `if (pakka_format_is_zip(...))`
# block. The opening `{` typically sits on the same line as the `if`,
# so we process that line's braces first (without printing it) and
# count any subsequent `{...}` nesting via a depth counter — without
# this, an inner `if (...) { ... }` on a single line collapses depth
# back to 0 and the lint stops scanning early.
zip_branch_lines() {
    local fn="$1"
    function_body "$fn" | awk '
        /if \(pakka_format_is_zip/ {
            in_zip = 1
            depth = 0
            for (i = 1; i <= length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") depth++
                else if (c == "}") depth--
            }
            next
        }
        in_zip {
            for (i = 1; i <= length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") depth++
                else if (c == "}") {
                    depth--
                    if (depth == 0) { in_zip = 0; next }
                }
            }
            print
        }
    '
}

findings=0
for fn in pakka_add_file pakka_add_memory; do
    branch=$(zip_branch_lines "$fn")
    [ -z "$branch" ] && continue
    for sym in "${GUARD_SYMBOLS[@]}"; do
        if echo "$branch" | grep -q "$sym"; then
            echo "advisory-lint: $sym referenced inside $fn's ZIP dispatch branch"
            echo "  This is the H1 regression class. The check should live in pak_add_preflight"
            echo "  or pak_add_source_preflight above the dispatch, not inside the branch."
            findings=$((findings + 1))
        fi
    done
done

if [ "$findings" -gt 0 ]; then
    echo ""
    echo "advisory-lint: $findings finding(s)."
    if [ "${STRICT:-0}" = "1" ]; then
        exit 1
    fi
fi
exit 0
