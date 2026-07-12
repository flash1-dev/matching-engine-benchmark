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


HARNESS_TIMEOUT_S = 1200   # outer backstop; the harness itself has a 600 s
                           # per-phase watchdog, so this only fires if a run is
                           # wedged past every internal guard.

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
    try:
        proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True,
                              timeout=HARNESS_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        # The harness has its own 600 s per-phase watchdog; if the whole
        # invocation still exceeds this outer bound the process is wedged —
        # surface the trial as ERROR instead of hanging the challenge forever.
        sys.stderr.write(f"ERROR: harness timed out ({HARNESS_TIMEOUT_S}s): {' '.join(cmd)}\n")
        return {"_returncode": -1, "engine": engine_label(spec),
                "scenario": scenario, "mode": mode, "verdict": "ERROR"}
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
    """The engine's definitional result under the worst-case framing.

    A scenario that produced no valid measurement (every perf trial
    crashed/ERRORed, so `median` is None) or that came back with a non-VALID
    verdict (a hash divergence, a failed audit, a correctness FAIL, ...) IS
    the worst possible outcome for that engine — it must not be silently
    excluded from the min just because it has no number to compare against
    the scenarios that did produce one. An engine that crashes on its hardest
    regime does not get to be graded only on the regimes it survived. Only
    when every scenario was measured AND VALID does the numeric minimum
    across medians become the definitional worst case.

    Returns (worst_mps, weakest_scenario, invalid_scenarios, kind) where kind
    is "measured" (worst_mps is a genuine median), "crash" (the weakest
    scenario had no valid measurement at all), or "invalid" (the weakest
    scenario measured a throughput but its verdict was not VALID — worst_mps
    is reported as 0.0 rather than that untrusted number). Returns None only
    if `scenarios` is empty."""
    cells = [(s, grid[(label, s)]) for s in scenarios]
    if not cells:
        return None
    invalid = [s for s, c in cells if c["verdict"] != "VALID"]
    crashed = [s for s, c in cells if c["median"] is None]
    if crashed:
        # Name the first (scenario order is fixed) rather than picking among
        # ties — the point is that a crash always wins the "weakest" title.
        return 0.0, crashed[0], invalid, "crash"
    if invalid:
        return 0.0, invalid[0], invalid, "invalid"
    weakest, worst_mps = min(((s, c["median"]) for s, c in cells),
                              key=lambda sc: sc[1])
    return worst_mps, weakest, invalid, "measured"


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
            worst_mps, weakest, invalid, kind = wc
            print("\nWorst-case throughput — the definitional result "
                  "(an engine is only as fast as the regime it handles worst):")
            if kind == "crash":
                print(f"  CRASH/INFEASIBLE on `{weakest}` — no valid "
                      f"measurement (every perf trial errored)")
            elif kind == "invalid":
                print(f"  INVALID on `{weakest}` — measured but its verdict "
                      f"was not VALID (untrusted; see below)")
            else:
                print(f"  {fmt_mps(worst_mps)} M/s  on `{weakest}`")
            if not invalid:
                print("  Verdict: VALID on all five scenarios")
            else:
                # Distinguish a crash/no-output scenario from a genuine hash
                # divergence — a crash never produced output to diverge.
                crashed = [s for s in invalid if grid[(lbl, s)]["median"] is None]
                diverged = [s for s in invalid if s not in crashed]
                reasons = []
                if crashed:
                    reasons.append("crashed/no valid measurement on "
                                    + ", ".join(crashed))
                if diverged:
                    reasons.append("output diverges on " + ", ".join(diverged))
                print("  Verdict: INVALID — " + "; ".join(reasons))
    else:
        for s in scenarios:
            print(f"\nScenario: {s}")
            print_table(["Engine"] + HEADERS[1:],
                        [summary_row(grid[(engine_label(e), s)], engine_label(e))
                         for e in engines])
        if len(scenarios) > 1:
            print("\nWorst-case ranking — the definitional result, lowest of each "
                  "engine's five scenarios (a venue must survive its worst regime):")
            # An engine with nothing measured (every scenario crashed) must
            # sort LAST, not first: worst_case() now reports 0.0 for that case
            # (the worst possible outcome, never excluded from comparison), so
            # the "nothing measured" fallback key must be the lowest possible
            # value too — float("-inf"), not "inf" — to stay consistent under
            # reverse=True descending order. (wc is only None when `scenarios`
            # is empty, which can't happen here since this branch requires
            # len(scenarios) > 1; kept as a defensive fallback.)
            ranked = sorted(
                ((engine_label(e), worst_case(grid, engine_label(e), scenarios))
                 for e in engines),
                key=lambda x: (x[1][0] if x[1] else float("-inf")), reverse=True)
            rows = []
            for lbl, wc in ranked:
                if wc is None:
                    rows.append((lbl, "n/a", "—", "—"))
                else:
                    worst_mps, weakest, invalid, kind = wc
                    mps_cell = ("CRASH" if kind == "crash"
                                else "INVALID" if kind == "invalid"
                                else fmt_mps(worst_mps))
                    rows.append((lbl, mps_cell, weakest,
                                 "VALID" if not invalid
                                 else f"INVALID ({len(invalid)}/{len(scenarios)})"))
            print_table(["Engine", "Worst-case M/s", "Weakest scenario", "Verdict"],
                        rows)

    sys.exit(0 if all(c["verdict"] == "VALID" for c in grid.values()) else 1)


if __name__ == "__main__":
    main()
