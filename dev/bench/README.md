# Local benchmark harness

A developer tool for timing the pakka CLI against a fixed workload table,
with optional cross-ref comparison driven by side git worktrees.

Not wired into CI — shared GitHub runners are too noisy for trustworthy
perf numbers. Run on your own machine and read the deltas, not the
absolute milliseconds.

## Prerequisites

- [`hyperfine`](https://github.com/sharkdp/hyperfine) — the timing
  engine. `brew install hyperfine` / `apt-get install hyperfine`.
- `python3` — used by `render.py` to merge JSON exports into the
  Markdown table.

## Usage

```sh
make bench                              # time the current build
make bench-compare REF=v1.5.0           # WIP vs v1.5.0
make bench-compare REF=master           # WIP vs master tip
make bench-clean                        # remove worktrees + cached output
```

`make bench` produces a single-column table in stdout. `make bench-compare
REF=<ref>` produces a side-by-side table with a Δ column (percent change
of WIP relative to the baseline; negative is faster).

Two refs:

```sh
dev/bench/compare.sh v1.5.0 v1.6.0      # both go through worktrees
```

## What it measures

Per-iteration mean and stddev over 10 timed runs (after 2 warmup runs)
for each of:

- Read-side: `pakka -l`, `pakka -l --tree`, `pakka --verify`,
  `pakka --verify --deep` (PK3 only), `pakka -x`.
- Write-side: `pakka -c` (from a pre-extracted source tree),
  `pakka -a --as`, `pakka -d`.

Each workload runs against:

- **Synthesized fixtures** (mandatory): 10k- and 50k-entry PAK and PK3
  archives, generated once and cached in `build/bench/synth/`. Synth is
  slow (50k PAK takes ~9 s due to libpakka's per-add duplicate-name
  scan being O(n)), but the cache is byte-stable so subsequent runs
  reuse it.
- **Real fixtures** (opportunistic): Q3 demo `pak0.pk3`, GoldSrc Uplink
  `pak0.pak`, Quake shareware `pak0.pak` — used if previously
  downloaded for the `make realpak-test-*` targets, silently skipped
  otherwise.

## Where output goes

```
build/bench/
  synth_tool             # the libpakka-driven archive generator
  synth/                 # cached synthesized archives
  scratch/               # per-run scratch (extract output, fresh copies)
  results/<label>/       # per-workload hyperfine JSON exports
  worktrees/<short-sha>/ # checkouts of baseline refs (compare.sh only)
```

The Markdown table is printed to stdout. Raw JSON stays in
`build/bench/results/<label>/<workload>.json` for anyone who wants to do
their own analysis.

## Reading the table

```
# Benchmark: HEAD-f15a0f7 vs v1.5.0-8c4d0e

| Workload          | HEAD-f15a0f7 (ms) | v1.5.0-8c4d0e (ms) | Δ        |
|-------------------|-------------------|--------------------|----------|
| list_pak10k       | 4.8 ± 0.2         | 7.1 ± 0.3          | -32.4%   |
| list_pak50k       | 22.3 ± 0.9        | 158.4 ± 4.2        | -85.9%   |
...
```

- Negative Δ means HEAD is faster.
- Stddev is the standard deviation across the 10 timed runs; a stddev
  comparable to the mean means the workload is noisy and the Δ should be
  taken with a grain of salt.
- `n/a` shows up when a workload was skipped (fixture missing) or
  errored out on one side.

## Cleanup

`make bench-clean` does the right thing in the common case: removes
every worktree this script made, prunes git's worktree registry, and
clears `build/bench/`.

### Interaction with `make distclean`

`make distclean` removes `build/` wholesale. If a worktree under
`build/bench/worktrees/` was registered with git, this leaves the
registry pointing at a gone path. Recovery:

```sh
git worktree prune
```

`make bench-clean` should ideally run before `make distclean`, but the
prune above also recovers.

## Why it's local-only

GitHub Actions runners have:

- Variable CPU allocation (shared vCPUs, throttling under load).
- No control over the noisy neighbor processes that share the host.
- Cold filesystem caches that vary per run.

A 5% perf regression isn't detectable against that noise floor, so a CI
benchmark would either flap (run-to-run variation > the regression
threshold) or paper over real regressions (threshold loose enough that
real ones slip past). Local-only benchmarking on a quiescent dev
machine is more useful for the questions this harness answers.

If perf-on-a-dedicated-box becomes desirable later, this harness emits
the JSON that a CI workflow could upload — the format isn't the
blocker, the runner is.
