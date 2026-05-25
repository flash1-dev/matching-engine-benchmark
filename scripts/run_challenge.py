#!/usr/bin/env python3
"""
run_challenge.py — drive the matching-engine benchmark harness.

A full challenge for one engine + scenario is N perf runs + 1 audit run
(docs/METHODOLOGY.md, default N = 10). This script runs them, reports the
median throughput, and prints the overall verdict: VALID only if every perf run
and the audit run are VALID and all runs agree on the report-stream hash.

Examples:
  scripts/run_challenge.py --baseline liquibook
  scripts/run_challenge.py --engine ./my_engine.so --scenario flash-crash
  scripts/run_challenge.py --engine ./cpptrader_adapter.so   # any additional_references/ adapter
  scripts/run_challenge.py --baseline quantcup --all-scenarios
  scripts/run_challenge.py --compare liquibook quantcup exchange_core
  scripts/run_challenge.py --compare ./my_engine.so liquibook --all-scenarios

Exit status is non-zero if any challenge's verdict is not VALID.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(REPO, "harness")
SCENARIOS = ["static", "normal", "swing-25", "swing-40", "flash-crash"]
DEFAULT_PERF_RUNS = 10


def engine_label(spec):
    """Human-readable engine name for a baseline name or a .so path."""
    base = os.path.basename(spec)
    for suf in ("_adapter.so", ".so", "_adapter"):
        if base.endswith(suf):
            base = base[: -len(suf)]
    return base


def is_path(spec):
    """True if `spec` is a .so path rather than a baseline name."""
    return spec.endswith(".so") or "/" in spec


def run_harness(spec, scenario, mode, args):
    """Invoke ./harness once for (spec, scenario, mode); return the result dict."""
    cmd = [HARNESS, "--scenario", scenario, "--mode", mode]
    cmd += ["--engine", spec] if is_path(spec) else ["--baseline", spec]
    if args.seed is not None:
        cmd += ["--seed", str(args.seed)]
    if args.count is not None:
        cmd += ["--count", str(args.count)]
    if args.matcher_core is not None:
        cmd += ["--matcher-core", str(args.matcher_core)]
    if args.drainer_core is not None:
        cmd += ["--drainer-core", str(args.drainer_core)]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)

    result_path = None
    for line in proc.stdout.splitlines():
        if line.startswith("Result file:"):
            result_path = line.split(":", 1)[1].strip()
    if result_path:
        full = os.path.join(REPO, result_path)
        if os.path.exists(full):
            try:
                with open(full) as f:
                    j = json.load(f)
            except (OSError, json.JSONDecodeError) as e:
                # A truncated or malformed result JSON (e.g., disk full,
                # interrupted write) would otherwise crash the whole challenge
                # run via JSONDecodeError. Surface this trial as ERROR and let
                # the remaining trials proceed.
                sys.stderr.write(
                    f"ERROR: could not parse {full}: {e}\n")
                return {"_returncode": proc.returncode,
                        "engine": engine_label(spec),
                        "scenario": scenario, "mode": mode, "verdict": "ERROR"}
            j["_returncode"] = proc.returncode
            return j
    # No result file: harness crashed or rejected its arguments.
    return {"_returncode": proc.returncode, "engine": engine_label(spec),
            "scenario": scenario, "mode": mode, "verdict": "ERROR"}


def challenge(spec, scenario, args):
    """Run `args.perf_runs` perf runs + 1 audit run; return a summary dict."""
    perf = [run_harness(spec, scenario, "perf", args)
            for _ in range(args.perf_runs)]
    audit = run_harness(spec, scenario, "audit", args)

    # Only count throughput from trials that produced a real result file with a
    # measured throughput. ERROR trials (crash, parse failure) have no measured
    # throughput; including them as 0 would skew the median into a lying figure.
    tputs = [j["throughput_msgs_per_s"] / 1e6 for j in perf
             if j.get("verdict") in ("VALID", "INVALID")
             and "throughput_msgs_per_s" in j]

    # Every run replays the same workload, so a deterministic engine produces
    # the identical report-stream hash on all of them. A disagreement is a real
    # fault.
    hashes = [j.get("correctness", {}).get("computed_hash", "?")
              for j in perf + [audit]]
    consistent = len(set(hashes)) == 1

    perf_ok  = all(j.get("verdict") == "VALID" for j in perf)
    audit_ok = audit.get("verdict") == "VALID"
    valid    = perf_ok and audit_ok and consistent

    a = audit.get("audit", {})
    audit_status = ("PASS" if a.get("passed")
                    else "FAIL" if a.get("ran") else
                    "ERROR" if audit.get("verdict") == "ERROR" else "SKIPPED")

    statuses = {j.get("correctness", {}).get("status", "?") for j in perf}
    if not consistent:
        correct = "INCONSISTENT"
    elif "FAIL" in statuses:
        correct = "FAIL"
    elif statuses == {"PASS"}:
        correct = "PASS"
    elif statuses == {"NO REFERENCE"}:
        correct = "NO REF"
    else:
        correct = "/".join(sorted(statuses))

    return {
        "engine":   engine_label(spec),
        "scenario": scenario,
        "median":   statistics.median(tputs) if tputs else None,
        "stdev":    statistics.pstdev(tputs) if len(tputs) > 1 else 0.0,
        "n_perf":   len(tputs),
        "trades":   perf[0].get("reports", {}).get("trade", "?") if perf else "?",
        "correct":  correct,
        "audit":    audit_status,
        "verdict":  "VALID" if valid else "INVALID",
    }


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


def summary_row(c, first):
    """A table row: `first` is the engine label or scenario name."""
    if c["median"] is None:
        mps_cell = "n/a"
    else:
        mps_cell = f"{c['median']:.2f} ± {c['stdev']:.2f}"
    return (first, mps_cell,
            str(c["trades"]), c["correct"], c["audit"], c["verdict"])


HEADERS = ["", "M/s (median±sd)", "Trades", "Correct", "Audit", "Verdict"]


def main():
    ap = argparse.ArgumentParser(
        description="Run the matching-engine benchmark challenge "
                    "(N perf runs + 1 audit run per scenario).")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--engine", metavar="PATH", help="engine .so to benchmark")
    g.add_argument("--baseline", metavar="NAME",
                   help="pre-built baseline: liquibook | quantcup | exchange_core")
    g.add_argument("--compare", nargs="+", metavar="ENGINE",
                   help="compare several engines (baseline names and/or .so paths)")
    ap.add_argument("--scenario", default="normal", choices=SCENARIOS)
    ap.add_argument("--all-scenarios", action="store_true",
                    help="run every scenario instead of just one")
    ap.add_argument("--seed", type=int, help="workload seed (default 12345)")
    ap.add_argument("--count", type=int, help="new-order count (default 1000000)")
    ap.add_argument("--perf-runs", type=int, default=DEFAULT_PERF_RUNS,
                    help=f"perf runs per scenario (default {DEFAULT_PERF_RUNS})")
    ap.add_argument("--matcher-core", type=int, help="pin the matcher thread")
    ap.add_argument("--drainer-core", type=int, help="pin the drainer thread")
    args = ap.parse_args()

    if not os.path.exists(HARNESS):
        sys.exit("error: ./harness is not built — run `make` first")

    scenarios = SCENARIOS if args.all_scenarios else [args.scenario]
    engines = args.compare if args.compare else [args.engine or args.baseline]

    print(f"Challenge: {args.perf_runs} perf runs + 1 audit run per scenario\n")
    grid = {}
    for e in engines:
        for s in scenarios:
            print(f"  running {engine_label(e)} / {s} "
                  f"({args.perf_runs} perf + 1 audit) ...", flush=True)
            grid[(engine_label(e), s)] = challenge(e, s, args)

    if len(engines) == 1:
        e = engines[0]
        print(f"\nSummary — {engine_label(e)}")
        print_table(["Scenario"] + HEADERS[1:],
                    [summary_row(grid[(engine_label(e), s)], s)
                     for s in scenarios])
    else:
        for s in scenarios:
            print(f"\nScenario: {s}")
            print_table(["Engine"] + HEADERS[1:],
                        [summary_row(grid[(engine_label(e), s)], engine_label(e))
                         for e in engines])

    sys.exit(0 if all(c["verdict"] == "VALID" for c in grid.values()) else 1)


if __name__ == "__main__":
    main()
