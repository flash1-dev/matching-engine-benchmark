#!/usr/bin/env python3
"""
run_challenge.py — drive the matching-engine benchmark harness.

A full challenge for one engine + scenario is N perf runs + 1 audit run
(docs/METHODOLOGY.md, default N = 10), yielding a median throughput and a
VALID/INVALID verdict for that scenario. By default this script runs all five
scenarios and reports the engine's **worst-case** throughput — the lowest of the
five, with the scenario that produces it — as the engine's definitional result,
because a venue must survive its worst regime, not its best. Pass --scenario to
measure a single scenario instead.

Examples:
  scripts/run_challenge.py --baseline liquibook                # all 5 + worst case
  scripts/run_challenge.py --engine ./cpptrader_adapter.so     # a third-party adapter .so (built from additional_references/)
  scripts/run_challenge.py --engine ./my_engine.so --scenario flash-crash  # one scenario
  scripts/run_challenge.py --compare liquibook quantcup exchange_core       # worst-case ranking

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


def fmt_mps(x):
    """Throughput in M/s — 2 decimals, but 2 significant figures below 0.1 so a
    sub-0.01 M/s engine is not rounded up to 0.01."""
    if x is None:
        return "n/a"
    return f"{x:.2f}" if x >= 0.1 else f"{x:.2g}"


def worst_case(grid, label, scenarios):
    """The engine's definitional result under the worst-case framing: the lowest
    median throughput across `scenarios`, the scenario that produces it, and the
    list of scenarios on which the engine is not VALID. Returns
    (worst_mps, weakest_scenario, invalid_scenarios), or None if nothing measured."""
    cells = [(s, grid[(label, s)]) for s in scenarios]
    measured = [(s, c["median"]) for s, c in cells if c["median"] is not None]
    if not measured:
        return None
    weakest, worst_mps = min(measured, key=lambda sc: sc[1])
    invalid = [s for s, c in cells if c["verdict"] != "VALID"]
    return worst_mps, weakest, invalid


def summary_row(c, first):
    """A table row: `first` is the engine label or scenario name."""
    mps_cell = ("n/a" if c["median"] is None
                else f"{fmt_mps(c['median'])} ± {c['stdev']:.2f}")
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
    ap.add_argument("--scenario", choices=SCENARIOS,
                    help="run a single scenario only (default: all five, "
                         "reporting the worst case as the definitional result)")
    ap.add_argument("--all-scenarios", action="store_true",
                    help="run every scenario (this is the default; kept as an "
                         "explicit override when --scenario is also given)")
    ap.add_argument("--seed", type=int, help="workload seed (default 23, the canonical seed)")
    ap.add_argument("--count", type=int, help="new-order count (default 1000000)")
    ap.add_argument("--perf-runs", type=int, default=DEFAULT_PERF_RUNS,
                    help=f"perf runs per scenario (default {DEFAULT_PERF_RUNS})")
    ap.add_argument("--matcher-core", type=int, help="pin the matcher thread")
    ap.add_argument("--drainer-core", type=int, help="pin the drainer thread")
    args = ap.parse_args()

    if not os.path.exists(HARNESS):
        sys.exit("error: ./harness is not built — run `make` first")

    # Default: run all five scenarios and report the worst case. A single
    # --scenario narrows to just that one (no worst-case line then).
    scenarios = (SCENARIOS if (args.all_scenarios or not args.scenario)
                 else [args.scenario])
    engines = args.compare if args.compare else [args.engine or args.baseline]

    print(f"Challenge: {args.perf_runs} perf runs + 1 audit run per scenario\n")
    grid = {}
    for e in engines:
        for s in scenarios:
            print(f"  running {engine_label(e)} / {s} "
                  f"({args.perf_runs} perf + 1 audit) ...", flush=True)
            grid[(engine_label(e), s)] = challenge(e, s, args)

    if len(engines) == 1:
        lbl = engine_label(engines[0])
        print(f"\nSummary — {lbl}")
        print_table(["Scenario"] + HEADERS[1:],
                    [summary_row(grid[(lbl, s)], s) for s in scenarios])
        wc = worst_case(grid, lbl, scenarios) if len(scenarios) > 1 else None
        if wc:
            worst_mps, weakest, invalid = wc
            print("\nWorst-case throughput — the definitional result "
                  "(an engine is only as fast as the regime it handles worst):")
            print(f"  {fmt_mps(worst_mps)} M/s  on `{weakest}`")
            print("  Verdict: " + ("VALID on all five scenarios" if not invalid
                                   else "INVALID — output diverges on "
                                        + ", ".join(invalid)))
    else:
        for s in scenarios:
            print(f"\nScenario: {s}")
            print_table(["Engine"] + HEADERS[1:],
                        [summary_row(grid[(engine_label(e), s)], engine_label(e))
                         for e in engines])
        if len(scenarios) > 1:
            print("\nWorst-case ranking — the definitional result, lowest of each "
                  "engine's five scenarios (a venue must survive its worst regime):")
            ranked = sorted(
                ((engine_label(e), worst_case(grid, engine_label(e), scenarios))
                 for e in engines),
                key=lambda x: (x[1][0] if x[1] else float("inf")), reverse=True)
            rows = []
            for lbl, wc in ranked:
                if wc is None:
                    rows.append((lbl, "n/a", "—", "—"))
                else:
                    worst_mps, weakest, invalid = wc
                    rows.append((lbl, fmt_mps(worst_mps), weakest,
                                 "VALID" if not invalid
                                 else f"INVALID ({len(invalid)}/{len(scenarios)})"))
            print_table(["Engine", "Worst-case M/s", "Weakest scenario", "Verdict"],
                        rows)

    sys.exit(0 if all(c["verdict"] == "VALID" for c in grid.values()) else 1)


if __name__ == "__main__":
    main()
