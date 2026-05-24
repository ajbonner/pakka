#!/usr/bin/env python3
"""Render hyperfine per-workload JSON exports into a Markdown table.

Usage:
    dev/bench/render.py <results-dir> [<baseline-results-dir>]

With one directory: prints a single-column table of mean ± stddev.
With two: prints a side-by-side table with a Δ column showing
percent change of the first directory relative to the second
(the baseline). Negative Δ = first directory is faster.

Each directory is expected to contain one JSON file per workload
(produced by `dev/bench/run.sh`). The basename of the directory is
the column label.

Skipped or failed workloads (JSON with `mean: null`) render as
"n/a" so they don't drop out of the table.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def load_dir(d: Path) -> dict[str, dict | None]:
    """Return {workload_name: hyperfine result dict | None} for one results dir."""
    out: dict[str, dict | None] = {}
    for p in sorted(d.glob("*.json")):
        with p.open() as f:
            j = json.load(f)
        results = j.get("results") or []
        if not results or results[0].get("mean") is None:
            out[p.stem] = None
        else:
            out[p.stem] = results[0]
    return out


def fmt_ms(r: dict | None) -> str:
    if r is None:
        return "n/a"
    return f"{r['mean'] * 1000:.1f} ± {r['stddev'] * 1000:.1f}"


def delta(head: dict | None, base: dict | None) -> str:
    if head is None or base is None:
        return "n/a"
    if base["mean"] == 0:
        return "n/a"
    pct = (head["mean"] - base["mean"]) / base["mean"] * 100.0
    sign = "+" if pct >= 0 else ""
    return f"{sign}{pct:.1f}%"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dirs", nargs="+", type=Path,
                    help="One or two results directories (HEAD first, baseline second)")
    args = ap.parse_args()

    if len(args.dirs) > 2:
        print("render.py: at most 2 directories supported", file=sys.stderr)
        return 1
    for d in args.dirs:
        if not d.is_dir():
            print(f"render.py: not a directory: {d}", file=sys.stderr)
            return 1

    labels = [d.name for d in args.dirs]
    data = [load_dir(d) for d in args.dirs]

    # Union of workload names across all dirs so a workload missing
    # from one column still shows as "n/a" rather than vanishing.
    workloads = sorted(set().union(*(d.keys() for d in data)))

    # Column widths
    name_w = max(len("Workload"), *(len(w) for w in workloads))
    col_w = max(15, *(len(label) for label in labels))

    if len(args.dirs) == 1:
        print(f"\n# Benchmark results — {labels[0]}\n")
        print(f"| {'Workload':<{name_w}} | {labels[0]:<{col_w}} (ms) |")
        print(f"|{'-' * (name_w + 2)}|{'-' * (col_w + 6)}|")
        for w in workloads:
            print(f"| {w:<{name_w}} | {fmt_ms(data[0].get(w)):<{col_w}}      |")
        return 0

    # Two columns + Δ
    print(f"\n# Benchmark: {labels[0]} vs {labels[1]}\n")
    head_label = f"{labels[0]} (ms)"
    base_label = f"{labels[1]} (ms)"
    delta_w = 8
    print(f"| {'Workload':<{name_w}} | {head_label:<{col_w}} | {base_label:<{col_w}} | {'Δ':<{delta_w}} |")
    print(f"|{'-' * (name_w + 2)}|{'-' * (col_w + 2)}|{'-' * (col_w + 2)}|{'-' * (delta_w + 2)}|")
    for w in workloads:
        h = data[0].get(w)
        b = data[1].get(w)
        print(f"| {w:<{name_w}} | {fmt_ms(h):<{col_w}} | {fmt_ms(b):<{col_w}} | {delta(h, b):<{delta_w}} |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
