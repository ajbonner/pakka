#!/bin/sh
# Compare the current pakka binary against an older git ref.
#
# Usage:
#   dev/bench/compare.sh <baseline-ref>           # compare ./pakka against <baseline>
#   dev/bench/compare.sh <baseline-ref> <head-ref> # both sides from worktrees
#   dev/bench/compare.sh --clean                  # remove every worktree this script made
#
# The default form is the common case: you're developing on the parent
# checkout, want to know "is my WIP faster than v1.5.0?" — no need to
# round-trip your current uncommitted changes through a worktree.
#
# Worktrees live under build/bench/worktrees/<short-sha>/ and each has
# its own build/ tree, so concurrent builds don't clobber the parent's
# build/. Worktrees are kept around after the run so a follow-up
# comparison against the same ref is instant; --clean removes them.

set -eu

cmd_clean() {
    if [ -d build/bench/worktrees ]; then
        for wt in build/bench/worktrees/*; do
            [ -d "$wt" ] || continue
            printf 'removing worktree %s\n' "$wt"
            # `git worktree remove` also unregisters; rm -rf would leak
            # an entry in `git worktree list`. Force in case the user
            # left build artifacts that look "dirty" to git.
            git worktree remove --force "$wt" 2>/dev/null \
                || rm -rf "$wt"
        done
        rmdir build/bench/worktrees 2>/dev/null || true
    fi
    # Sweep up any worktree registry entries pointing at gone paths
    # (e.g. after a `make distclean` removed build/ without going
    # through this command).
    git worktree prune
    printf 'compare.sh: clean complete\n'
}

if [ "${1:-}" = "--clean" ]; then
    cmd_clean
    exit 0
fi

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    cat >&2 <<EOF
Usage: $0 <baseline-ref> [head-ref]
       $0 --clean
EOF
    exit 1
fi

BASELINE_REF=$1
HEAD_REF=${2:-}

# Validate refs up front so we fail fast.
if ! git rev-parse --verify "$BASELINE_REF" >/dev/null 2>&1; then
    printf 'compare.sh: ref not found: %s\n' "$BASELINE_REF" >&2
    exit 1
fi
if [ -n "$HEAD_REF" ] && ! git rev-parse --verify "$HEAD_REF" >/dev/null 2>&1; then
    printf 'compare.sh: ref not found: %s\n' "$HEAD_REF" >&2
    exit 1
fi

WORKTREE_BASE=build/bench/worktrees
mkdir -p "$WORKTREE_BASE"

# Each worktree is keyed by its resolved short-sha so 'master' and
# 'HEAD' don't end up in different trees when they point at the same
# commit. Tags also resolve stably this way.
short_sha() {
    git rev-parse --short "$1"
}

# Build inside a worktree, return the binary path on stdout.
# Every user-facing message goes to stderr so the caller can safely
# $(build_worktree ...) the binary path without picking up status lines.
build_worktree() {
    sha=$1
    wt=$WORKTREE_BASE/$sha
    if [ ! -d "$wt" ]; then
        printf '\n== Worktree for %s (%s) ==\n' "$2" "$sha" >&2
        git worktree add "$wt" "$sha" >&2
    fi
    if [ ! -x "$wt/pakka" ] \
        || [ "$wt/src/pakfile.c" -nt "$wt/pakka" ] 2>/dev/null; then
        printf 'building in %s ...\n' "$wt" >&2
        (cd "$wt" && make -j) >&2
    fi
    printf '%s/pakka\n' "$wt"
}

# Always run the *parent's* bench scripts (run.sh, render.py, synth_tool).
# Old refs may not have dev/bench/ at all; we only need their binary.
SYNTH_TOOL=build/bench/synth_tool
if [ ! -x "$SYNTH_TOOL" ]; then
    printf 'compare.sh: building synth_tool ...\n'
    make build/bench/synth_tool >/dev/null
fi

# --------------------------------------------------------------
# Run the head side.
# --------------------------------------------------------------
if [ -z "$HEAD_REF" ]; then
    if [ ! -x ./pakka ]; then
        printf 'compare.sh: ./pakka not built; run `make` first.\n' >&2
        exit 1
    fi
    HEAD_SHA=$(git rev-parse --short HEAD)
    HEAD_LABEL="HEAD-$HEAD_SHA"
    HEAD_BIN=./pakka
    printf '\n== Running bench: HEAD (%s, current build) ==\n' "$HEAD_SHA"
else
    HEAD_SHA=$(short_sha "$HEAD_REF")
    HEAD_LABEL="$HEAD_REF-$HEAD_SHA"
    HEAD_BIN=$(build_worktree "$HEAD_SHA" "$HEAD_REF")
    printf '\n== Running bench: %s (%s) ==\n' "$HEAD_REF" "$HEAD_SHA"
fi
dev/bench/run.sh "$HEAD_BIN" "$SYNTH_TOOL" "$HEAD_LABEL"

# --------------------------------------------------------------
# Run the baseline side.
# --------------------------------------------------------------
BASELINE_SHA=$(short_sha "$BASELINE_REF")
BASELINE_LABEL="$BASELINE_REF-$BASELINE_SHA"
BASELINE_BIN=$(build_worktree "$BASELINE_SHA" "$BASELINE_REF")
printf '\n== Running bench: %s (%s) ==\n' "$BASELINE_REF" "$BASELINE_SHA"
dev/bench/run.sh "$BASELINE_BIN" "$SYNTH_TOOL" "$BASELINE_LABEL"

# --------------------------------------------------------------
# Render.
# --------------------------------------------------------------
printf '\n'
dev/bench/render.py \
    "build/bench/results/$HEAD_LABEL" \
    "build/bench/results/$BASELINE_LABEL"
