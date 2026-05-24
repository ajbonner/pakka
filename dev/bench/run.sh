#!/bin/sh
# Run the bench workload table against a single pakka binary, exporting
# per-workload JSON to build/bench/results/<label>/.
#
# Usage:
#   dev/bench/run.sh <pakka-binary> <synth-tool-binary> [label]
#
# `label` defaults to the binary's basename ('pakka') and names the
# results subdirectory. compare.sh passes the short-sha so multiple runs
# don't clobber each other.
#
# Always invoked from the parent checkout — never from inside a
# build/bench/worktrees/<sha>/ tree. Old refs may not have this file.

set -eu

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    printf 'Usage: %s <pakka> <synth_tool> [label]\n' "$0" >&2
    exit 1
fi

PAKKA=$1
SYNTH=$2
LABEL=${3:-pakka}

if [ ! -x "$PAKKA" ]; then
    printf 'run.sh: pakka binary not executable: %s\n' "$PAKKA" >&2
    exit 1
fi
if [ ! -x "$SYNTH" ]; then
    printf 'run.sh: synth_tool not executable: %s\n' "$SYNTH" >&2
    exit 1
fi

if ! command -v hyperfine >/dev/null 2>&1; then
    cat >&2 <<EOF
run.sh: hyperfine not found in PATH.

Install:
  macOS:   brew install hyperfine
  Debian:  apt-get install hyperfine
  Or download a release from https://github.com/sharkdp/hyperfine/releases

The bench harness is local-only and not wired into CI, so the dependency
is on the developer's machine, not the build matrix.
EOF
    exit 1
fi

BENCH_DIR=build/bench
SYNTH_CACHE=$BENCH_DIR/synth
RESULTS_DIR=$BENCH_DIR/results/$LABEL
SCRATCH=$BENCH_DIR/scratch

mkdir -p "$SYNTH_CACHE" "$RESULTS_DIR" "$SCRATCH"

# ------------------------------------------------------------------
# Materialize synth archives lazily — generate-once, cache-forever.
# Synth is slow (10k PAK ~0.3s, 50k PAK ~9s) due to the per-add O(n)
# duplicate-name scan in pakfile.c, which makes the whole synth O(n^2).
# Cached output is byte-stable across runs so re-using build/bench/synth/
# from a prior session is safe.
# ------------------------------------------------------------------
ensure_synth() {
    fmt=$1
    count=$2
    out=$3
    if [ -f "$out" ]; then return 0; fi
    printf '  synthesizing %s entries -> %s ... ' "$count" "$out"
    t0=$(date +%s)
    "$SYNTH" "$fmt" "$count" "$out" >/dev/null
    t1=$(date +%s)
    printf '%ss\n' "$((t1 - t0))"
}

PAK_10K=$SYNTH_CACHE/10k.pak
PAK_50K=$SYNTH_CACHE/50k.pak
PK3_10K=$SYNTH_CACHE/10k.pk3
PK3_50K=$SYNTH_CACHE/50k.pk3

printf '\n== Synth fixtures (cached in %s) ==\n' "$SYNTH_CACHE"
ensure_synth pak 10000 "$PAK_10K"
ensure_synth pak 50000 "$PAK_50K"
ensure_synth pk3 10000 "$PK3_10K"
ensure_synth pk3 50000 "$PK3_50K"

# Source directory for the `create` workload. Extracted once from the
# 10k PAK so it has 10k tiny files in a single subdir — recursing into
# it inside `pakka -c` exercises the same code path a user invoking
# `pakka -c new.pak src/` would hit.
SOURCE_DIR=$SCRATCH/source_10k
if [ ! -d "$SOURCE_DIR" ]; then
    printf '  extracting source tree for create workload ... '
    rm -rf "$SOURCE_DIR"
    mkdir -p "$SOURCE_DIR"
    "$PAKKA" -x -C "$SOURCE_DIR" "$PAK_10K" >/dev/null
    printf 'done (%s files)\n' "$(find "$SOURCE_DIR" -type f | wc -l | tr -d ' ')"
fi

# ------------------------------------------------------------------
# Workload table.
#
# Each row: NAME CMD [--prepare PREP]
#
# Hyperfine flags:
#   --warmup 2     two unmeasured runs to populate disk cache / dyld cache
#   --runs 10      ten measured iterations (enough for a stable mean at
#                  short wall-clock; reduce manually for long workloads
#                  if needed)
#   --export-json  per-workload JSON consumed by render.py
#
# Per-workload hyperfine invocation (not a batched multi-command call)
# so a failure on one workload doesn't poison the JSON for siblings.
# ------------------------------------------------------------------

# Preflight: confirm the target binary actually accepts the flags this
# bench uses. Older refs with renamed flags would otherwise fail mid-run.
preflight() {
    flag=$1
    if "$PAKKA" "$flag" --help >/dev/null 2>&1 \
        || "$PAKKA" --help 2>&1 | grep -q -- "$flag"; then
        return 0
    fi
    return 1
}

# Returns 0 if hyperfine should run this workload, 1 to skip.
should_run() {
    case $1 in
        *_q3demo) [ -f build/test/q3demo/pak0.pk3 ] ;;
        *_uplink) [ -f build/test/hl-uplink/valve/pak0.pak ] ;;
        *_quakesw) [ -f build/test/id1/pak0.pak ] ;;
        *) return 0 ;;
    esac
}

run_bench() {
    name=$1
    shift
    out=$RESULTS_DIR/$name.json
    if ! should_run "$name"; then
        printf '  [skip] %s (fixture not present)\n' "$name"
        # Emit a sentinel so render.py shows "n/a" rather than dropping
        # the row entirely from the table.
        printf '{"results":[{"command":"skipped","mean":null}]}\n' > "$out"
        return 0
    fi
    printf '  %s ... ' "$name"
    if hyperfine --warmup 2 --runs 10 --export-json "$out" \
            "$@" >/dev/null 2>&1; then
        mean=$(python3 -c "import json,sys;j=json.load(open('$out'));print(f\"{j['results'][0]['mean']*1000:.1f} ms ± {j['results'][0]['stddev']*1000:.1f}\")")
        printf '%s\n' "$mean"
    else
        printf 'FAILED (n/a)\n'
        printf '{"results":[{"command":"failed","mean":null}]}\n' > "$out"
    fi
}

printf '\n== Read-side workloads ==\n'

# Synthesized fixtures
for size in 10k 50k; do
    case $size in
        10k) pak=$PAK_10K; pk3=$PK3_10K ;;
        50k) pak=$PAK_50K; pk3=$PK3_50K ;;
    esac
    run_bench "list_pak$size"          "$PAKKA -l $pak"
    run_bench "tree_pak$size"          "$PAKKA -l --tree $pak"
    run_bench "verify_pak$size"        "$PAKKA --verify $pak"
    run_bench "list_pk3$size"          "$PAKKA -l $pk3"
    run_bench "tree_pk3$size"          "$PAKKA -l --tree $pk3"
    run_bench "verify_pk3$size"        "$PAKKA --verify $pk3"
    run_bench "verifydeep_pk3$size"    "$PAKKA --verify --deep $pk3"
done

# Real fixtures, opportunistic
run_bench "list_q3demo"       "$PAKKA -l build/test/q3demo/pak0.pk3"
run_bench "tree_q3demo"       "$PAKKA -l --tree build/test/q3demo/pak0.pk3"
run_bench "verify_q3demo"     "$PAKKA --verify build/test/q3demo/pak0.pk3"
run_bench "list_uplink"       "$PAKKA -l build/test/hl-uplink/valve/pak0.pak"
run_bench "tree_uplink"       "$PAKKA -l --tree build/test/hl-uplink/valve/pak0.pak"
run_bench "list_quakesw"      "$PAKKA -l build/test/id1/pak0.pak"

printf '\n== Extract (resets scratch dir each iteration) ==\n'

EXTRACT_SCRATCH=$SCRATCH/extract_out
run_bench "extract_pak10k" \
    --prepare "rm -rf '$EXTRACT_SCRATCH' && mkdir -p '$EXTRACT_SCRATCH'" \
    "$PAKKA -x -C $EXTRACT_SCRATCH $PAK_10K"
run_bench "extract_pk310k" \
    --prepare "rm -rf '$EXTRACT_SCRATCH' && mkdir -p '$EXTRACT_SCRATCH'" \
    "$PAKKA -x -C $EXTRACT_SCRATCH $PK3_10K"

printf '\n== Write-side workloads (fresh archive each iteration) ==\n'

# Create: needs the source dir (extracted above) and a fresh output path.
CREATE_OUT=$SCRATCH/create_out.pak
run_bench "create_pak_from10k" \
    --prepare "rm -f '$CREATE_OUT'" \
    "$PAKKA -c $CREATE_OUT $SOURCE_DIR"

CREATE_PK3_OUT=$SCRATCH/create_out.pk3
run_bench "create_pk3_from10k" \
    --prepare "rm -f '$CREATE_PK3_OUT'" \
    "$PAKKA -c $CREATE_PK3_OUT $SOURCE_DIR"

# Add one entry to a fresh copy of the synth archive.
ADD_TARGET=$SCRATCH/add_target.pak
EXTRA_SRC=$SCRATCH/extra.bin
printf 'extra payload' > "$EXTRA_SRC"
run_bench "add_one_pak10k" \
    --prepare "cp '$PAK_10K' '$ADD_TARGET'" \
    "$PAKKA -a $ADD_TARGET --as bench/zzz_extra.dat $EXTRA_SRC"

ADD_PK3_TARGET=$SCRATCH/add_target.pk3
run_bench "add_one_pk310k" \
    --prepare "cp '$PK3_10K' '$ADD_PK3_TARGET'" \
    "$PAKKA -a $ADD_PK3_TARGET --as bench/zzz_extra.dat $EXTRA_SRC"

# Delete one middle entry (forces PAK rebuild).
DEL_TARGET=$SCRATCH/del_target.pak
run_bench "delete_one_pak10k" \
    --prepare "cp '$PAK_10K' '$DEL_TARGET'" \
    "$PAKKA -d $DEL_TARGET bench/00005000.dat"

DEL_PK3_TARGET=$SCRATCH/del_target.pk3
run_bench "delete_one_pk310k" \
    --prepare "cp '$PK3_10K' '$DEL_PK3_TARGET'" \
    "$PAKKA -d $DEL_PK3_TARGET bench/00005000.dat"

printf '\nResults JSON: %s/\n' "$RESULTS_DIR"
