#!/usr/bin/env python3
"""
compare_results.py — tabulate matching-engine benchmark result files.

The harness writes one JSON file per run under results/. A challenge produces
N perf runs + 1 audit run per (engine, scenario); this script reads any number
of result JSONs (paths or globs), groups them by (engine, scenario, mode), and
prints a single comparison table with the median throughput across the group.

Examples:
  scripts/compare_results.py results/*.json
  scripts/compare_results.py results/liquibook_normal_perf_*.json \\
                             results/quantcup_normal_perf_*.json
"""
import glob
import json
import os
import statistics
import sys


def load(patterns):
    """Load every result JSON matching the given paths/globs."""
    out = []
    for pat in patterns:
        for path in sorted(glob.glob(pat)):
            try:
                with open(path) as fh:
                    j = json.load(fh)
            except (OSError, json.JSONDecodeError) as e:
                sys.stderr.write(f"skip {path}: {e}\n")
                continue
            j["_file"] = os.path.basename(path)
            out.append(j)
    return out


def print_table(headers, rows):
    widths = [len(h) for h in headers]
    for r in rows:
        for i, c in enumerate(r):
            widths[i] = max(widths[i], len(str(c)))
    fmt = lambda r: "  ".join(str(c).ljust(widths[i]) for i, c in enumerate(r))
    print(fmt(headers))
    print("  ".join("-" * w for w in widths))
    for r in rows:
        print(fmt(r))


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: compare_results.py <result.json> [...]")

    results = load(sys.argv[1:])
    if not results:
        sys.exit("no result files matched")

    # Group all result files by (engine, scenario, mode); within each group we
    # compute the median throughput (only counting trials with a measured M/s)
    # and use the most recent JSON for non-numeric fields.
    groups = {}
    for j in results:
        key = (j.get("engine", "?"), j.get("scenario", "?"), j.get("mode", "?"))
        groups.setdefault(key, []).append(j)

    rows = []
    for key in sorted(groups):
        entries = groups[key]
        latest = max(entries, key=lambda j: j["_file"])
        mode = latest.get("mode", "?")
        a = latest.get("audit", {})
        audit = "skip" if not a.get("ran") else ("PASS" if a.get("passed") else "FAIL")

        # Only count throughput from entries that actually produced a measured
        # M/s — drop ERROR-verdict files (which carry 0 or no throughput field).
        tputs = [j["throughput_msgs_per_s"] / 1e6 for j in entries
                 if j.get("verdict") in ("VALID", "INVALID")
                 and "throughput_msgs_per_s" in j]
        if mode == "perf" and tputs:
            n   = len(tputs)
            med = statistics.median(tputs)
            sd  = statistics.pstdev(tputs) if n > 1 else 0.0
            mps_cell = f"{med:.2f} ± {sd:.2f} (n={n})"
        elif mode == "perf":
            mps_cell = "n/a"
        else:
            mps_cell = "-"

        rows.append((
            latest.get("engine", "?"),
            latest.get("scenario", "?"),
            mode,
            mps_cell,
            str(latest.get("reports", {}).get("trade", "?")),
            latest.get("correctness", {}).get("status", "?"),
            audit if mode == "audit" else "-",
            latest.get("verdict", "?"),
        ))
    print_table(["Engine", "Scenario", "Mode", "M/s (median±sd)", "Trades",
                 "Correct", "Audit", "Verdict"], rows)


if __name__ == "__main__":
    main()
