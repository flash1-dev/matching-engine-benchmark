#!/usr/bin/env python3
"""
validate_seeds.py — check an engine against the published consensus hashes of the
random seeds in reference/correctness_hash.txt (listed in workload/seeds.txt).
Correctness only, built for CI pipelines.

For every selected (seed, scenario) cell the harness runs the engine once, in perf
mode, on that seed's 1,000,000-order workload. A cell passes when the SHA-256 of the
engine's full report stream equals the published line: the hash is read from the
result file the harness writes for that run and compared by this script, so nothing
the engine prints can change the outcome. The harness's own verdict is recorded but
not required (see --require-valid). Throughput is not measured: cells run in
parallel and unpinned (use scripts/run_challenge.py for throughput).

Seed selection
  default         validation mode: every published seed (seed 23, the canonical
                  seed, is not part of the pool; run_challenge.py covers it)
  --seeds N       N of the published seeds, drawn uniformly at random without
                  replacement; reproducible with the --sample-seed it prints (same
                  published file, same Python version; the JSON report lists the seeds)
  --seed-list     explicit published seeds (comma-separated, or @file with one per line)

Recommended CI use: a fresh random draw on every commit or pull request (--seeds 20
to 100 with --fail-fast; --estimate sizes it to your time budget), the full set
nightly or before a release (--junit for your CI dashboard), and every seed that
ever failed kept in a --seed-list @file that runs every time.

Examples:
  scripts/validate_seeds.py --engine ./my_engine.so                 # all published seeds x 5 scenarios
  scripts/validate_seeds.py --engine ./my_engine.so --seeds 100     # 100 random seeds (500 cells)
  scripts/validate_seeds.py --engine ./my_engine.so --seeds 50 --fail-fast --junit validation.xml
  scripts/validate_seeds.py --engine ./my_engine.so --seed-list @regression_seeds.txt
  scripts/validate_seeds.py --engine ./my_engine.so --estimate        # time a calibration sample, project, stop

Cost depends on the engine. A cell generates a ~2,000,000-message workload (about a
second) and runs the engine over it once, so a fast engine spends a few seconds per
cell and a 0.1 M msgs/s engine about half a minute; the full set (50,000 cells) takes
from under an hour on a large server to days. So the ETA is never a fixed rate: it is
projected per scenario from the cells this engine has finished in this run, at this
parallelism (provisional until every scenario has a finished cell and the first wave
of jobs has completed), and --estimate runs a calibration sample first and prints the
projected time for the selection and for other sample sizes, without running it.
Each job holds one ~80 MB workload file and one harness process of about 0.6 GB (more
for engines that hold large books); the default --jobs is capped by available memory.
Ctrl-C, SIGTERM and SIGHUP (how CI systems cancel a job) stop the run: running cells
are killed and their files removed, and the reports are written with them as not run.

Exit status: 0 = every selected cell passed (with --estimate: every calibration cell);
1 = at least one cell failed (FAIL, CRASH, TIMEOUT, ERROR, or INVALID under
--require-valid) or the run was stopped; 2 = setup error (missing harness or engine,
bad arguments).
"""
import argparse
import concurrent.futures
import glob
import json
import math
import os
import random
import re
import secrets
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(REPO, "harness")
GENERATOR = os.path.join(REPO, "generator")
HASH_FILE = os.path.join(REPO, "reference", "correctness_hash.txt")
RESULTS = os.path.join(REPO, "results")
SCENARIOS = ["static", "normal", "swing-25", "swing-40", "flash-crash"]
COUNT = 1_000_000
CANONICAL_SEED = 23
TAG_RE = re.compile(r"^(?P<scenario>[a-z0-9-]+)_seed(?P<seed>\d+)_count(?P<count>\d+)_reports\.txt$")
GB_PER_JOB = 0.75                 # one harness process (~0.55 GB measured) + its workload file + margin
TAIL_BYTES = 64 * 1024            # how much of a run's output is kept for a failure report
DIAG_RE = re.compile(r"\b(Anti-cheat|Affinity|Measurement|ERROR|WARNING)\b")
XML_BAD = re.compile("[^\x09\x0a\x0d\x20-퟿-�\U00010000-\U0010ffff]")

STOP = threading.Event()          # start no new cells (--fail-fast, or a signal)
INTERRUPTED = threading.Event()   # a signal: running cells are killed and count as not run
ACTIVE = {}                       # pid -> Popen of every running child process group
ACTIVE_LOCK = threading.Lock()


def die(msg):
    sys.stderr.write(f"validate_seeds: {msg}\n")
    sys.exit(2)


def load_published(scenarios):
    """({(seed, scenario): hash} at count 1,000,000, the pool: seeds != 23 that have every requested scenario)."""
    if not os.path.exists(HASH_FILE):
        die(f"missing {HASH_FILE}")
    published, have = {}, {}
    with open(HASH_FILE, errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]):
                continue
            m = TAG_RE.match(parts[1])
            if not m or int(m["count"]) != COUNT:
                continue
            key = (int(m["seed"]), m["scenario"])
            if key not in published:          # first entry wins, as in the harness
                published[key] = parts[0]
                have.setdefault(key[0], set()).add(key[1])
    pool = sorted(s for s, scs in have.items() if s != CANONICAL_SEED and set(scenarios) <= scs)
    return published, pool


def usable_cpus():
    try:
        return len(os.sched_getaffinity(0))      # respects taskset / container cpusets
    except AttributeError:
        return os.cpu_count() or 2


def mem_available_gb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 2 ** 20
    except (OSError, ValueError, IndexError):
        pass
    return None


def default_jobs():
    jobs = max(1, usable_cpus() // 2)
    mem = mem_available_gb()
    return max(1, min(jobs, int(mem / GB_PER_JOB))) if mem else jobs


def parse_seed_list(spec):
    if spec.startswith("@"):
        try:
            with open(spec[1:]) as f:
                text = f.read()
        except OSError as e:
            die(f"--seed-list: {e}")
        items = [l.split("#", 1)[0].strip() for l in text.splitlines()]
    else:
        items = [x.strip() for x in spec.split(",")]
    try:
        return [int(x) for x in items if x]
    except ValueError as e:
        die(f"--seed-list: {e}")


def kill_group(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def kill_active():
    with ACTIVE_LOCK:
        for pid in list(ACTIVE):
            kill_group(pid)


def run_group(cmd, timeout, out_path, err_path):
    """Run cmd in its own process group with its output in files (bounded memory; no pipe that a detached
    grandchild could hold open). On timeout the whole group is killed. Returns (returncode, pid);
    returncode is None when the run was killed for the timeout or stopped by a signal."""
    if INTERRUPTED.is_set():
        return None, None
    with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
        p = subprocess.Popen(cmd, cwd=REPO, stdin=subprocess.DEVNULL, stdout=fo, stderr=fe, start_new_session=True)
        with ACTIVE_LOCK:
            ACTIVE[p.pid] = p
        try:
            if INTERRUPTED.is_set():             # a signal arrived between the check above and the Popen
                kill_group(p.pid)
            try:
                rc = p.wait(timeout=max(0.1, timeout))
            except subprocess.TimeoutExpired:
                kill_group(p.pid)
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
                return None, p.pid
            return (None if INTERRUPTED.is_set() and rc < 0 else rc), p.pid
        finally:
            with ACTIVE_LOCK:
                ACTIVE.pop(p.pid, None)


def read_tail(path, n=TAIL_BYTES):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - n))
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def tape_ok(path):
    """A complete workload file: 16-byte header (magic, version, record count) + count x 40-byte records."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(16)
        n = int.from_bytes(hdr[12:16], "little") if len(hdr) == 16 else 0
        return n > 0 and os.path.getsize(path) == 16 + 40 * n
    except OSError:
        return False


def remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def result_files(scenario, seed, pid):
    """The harness's result file for this run: results/<engine>_<scenario>_perf_s<seed>_<time>_p<pid>.json, found by
    the harness's own process id (never by a path printed on stdout, which the engine shares)."""
    if not pid:
        return []
    real = os.path.realpath(RESULTS)
    return [f for f in glob.glob(os.path.join(RESULTS, f"*_{scenario}_perf_s{seed}_*_p{pid}.json"))
            if os.path.dirname(os.path.realpath(f)) == real]


def run_cell(engine, seed, scenario, args, published):
    """Generate the cell's workload (unless a complete one is present), run the harness once, decide the outcome
    from the harness's result file; returns a record. Removes the workload file and result file it created."""
    rec = {"seed": seed, "scenario": scenario, "expected": published[(seed, scenario)]}
    name = f"orders_{scenario}_s{seed}_n{COUNT}"
    tape = os.path.join(REPO, name + ".bin")
    tmp_tape = os.path.join(REPO, f"{name}.tmp{os.getpid()}_{threading.get_ident()}.bin")   # matches orders_*.bin
    created, pid = False, None
    t0 = time.time(); deadline = t0 + args.timeout
    with tempfile.TemporaryDirectory(prefix="validate_seeds_") as td:
        out_p, err_p = os.path.join(td, "stdout"), os.path.join(td, "stderr")
        try:
            if not tape_ok(tape):
                rc, _ = run_group([GENERATOR, scenario, tmp_tape, str(COUNT), str(seed)], deadline - time.time(),
                                  out_p, err_p)
                if rc == 0 and tape_ok(tmp_tape):
                    os.replace(tmp_tape, tape)       # atomic: a concurrent reader sees a complete file or none
                    created = True
                else:
                    rec["outcome"] = "NOT_RUN" if INTERRUPTED.is_set() else ("TIMEOUT" if rc is None else "ERROR")
                    rec["detail"] = ("workload generation " + ("timed out" if rc is None else f"failed (exit {rc})")
                                     + "\n" + read_tail(err_p, 2000)).strip()
                    return rec
            rec["generate_s"] = round(time.time() - t0, 2)
            cmd = [HARNESS, "--engine", engine, "--scenario", scenario, "--seed", str(seed), "--count", str(COUNT),
                   "--mode", "perf", "--matcher-core", "-1", "--drainer-core", "-1"]
            rc, pid = run_group(cmd, deadline - time.time(), out_p, err_p)
            out, err = read_tail(out_p), read_tail(err_p)
            rec["returncode"] = rc
            j = None
            files = result_files(scenario, seed, pid)
            if len(files) == 1:
                try:
                    with open(files[0]) as f:
                        j = json.load(f)
                except (OSError, ValueError):
                    j = None
            if isinstance(j, dict):
                corr = j.get("correctness") if isinstance(j.get("correctness"), dict) else {}
                tp = j.get("throughput_msgs_per_s")
                rec.update(computed=corr.get("computed_hash"), harness_status=corr.get("status"),
                           verdict=j.get("verdict"), probe_share=j.get("excluded_frac"),
                           throughput_mps=round(tp / 1e6, 3) if isinstance(tp, (int, float)) else None)
            if INTERRUPTED.is_set() and rc is None:
                outcome = "NOT_RUN"
            elif rc is None or "exceeded the time limit" in err:
                outcome = "TIMEOUT"
            elif rc == 3 or "engine crashed" in err:
                outcome = "CRASH"
            elif not isinstance(j, dict) or j.get("seed") != seed or j.get("scenario") != scenario:
                outcome = "ERROR"                     # no (or a foreign) result file
            elif rec.get("computed") == rec["expected"]:
                outcome = "PASS"
                if args.require_valid and (rec.get("verdict") != "VALID" or rc != 0):
                    outcome = "INVALID"
            else:
                outcome = "FAIL"
            rec["outcome"] = outcome
            if outcome not in ("PASS", "NOT_RUN"):
                diag = [l for l in out.splitlines() if DIAG_RE.search(l)][-8:]
                lines = diag + err.strip().splitlines()[-8:]
                rec["detail"] = ("\n".join(lines) if lines else out.strip())[-3000:]
        except Exception as e:                        # the validator's own failure must not end the run
            rec["outcome"] = "NOT_RUN" if INTERRUPTED.is_set() else "ERROR"
            rec["detail"] = f"validator error: {e!r}"
        finally:
            if not args.keep_results:
                for f in result_files(scenario, seed, pid):
                    remove(f)
            remove(tmp_tape)
            if created and not args.keep_tapes and seed != CANONICAL_SEED:
                remove(tape)
            rec["wall_s"] = round(time.time() - t0, 2)
    if rec["outcome"] not in ("PASS", "NOT_RUN"):
        rec["repro"] = (f"cd {shlex.quote(REPO)} && ./harness --engine {shlex.quote(engine)} --scenario {scenario} "
                        f"--seed {seed} --count {COUNT} --mode perf --matcher-core -1 --drainer-core -1")
    return rec


def fmt_dur(sec):
    if sec is None:
        return "?"
    if sec < 90:
        return f"{sec:.0f} s"
    if sec < 5400:
        return f"{sec / 60:.1f} min"
    if sec < 172800:
        return f"{sec / 3600:.1f} h"
    return f"{sec / 86400:.1f} days"


class CostModel:
    """Wall seconds per cell, per scenario, from the cells this engine has finished in this run (full engine runs:
    PASS, FAIL, INVALID; crashes and timeouts are not representative). A cell's cost is mostly the engine's own run
    over ~2M messages, so it varies by orders of magnitude between engines and by scenario for one engine; the
    measured walls already include the contention of the parallel jobs they ran beside."""

    def __init__(self, cells, jobs):
        self.planned = {}
        for _, sc in cells:
            self.planned[sc] = self.planned.get(sc, 0) + 1
        self.jobs, self.sum, self.n, self.done, self.rate = jobs, {}, {}, {}, {}

    def add(self, rec):
        sc = rec["scenario"]
        self.done[sc] = self.done.get(sc, 0) + 1
        if rec["outcome"] in ("PASS", "FAIL", "INVALID"):
            self.sum[sc] = self.sum.get(sc, 0.0) + rec["wall_s"]; self.n[sc] = self.n.get(sc, 0) + 1
            if rec.get("throughput_mps"):
                self.rate.setdefault(sc, []).append(rec["throughput_mps"])

    def calibrated(self):
        return all(self.n.get(sc) for sc in self.planned)

    def per_cell(self, sc):
        if self.n.get(sc):
            return self.sum[sc] / self.n[sc]
        n = sum(self.n.values())
        return sum(self.sum.values()) / n if n else None

    def seconds(self, cells_by_sc, jobs):
        """Projected wall time for cells_by_sc ({scenario: cells}) on `jobs` parallel jobs."""
        total = sum(k for k in cells_by_sc.values() if k > 0)
        if not total:
            return 0.0
        if any(self.per_cell(sc) is None for sc, k in cells_by_sc.items() if k > 0):
            return None
        return sum(k * self.per_cell(sc) for sc, k in cells_by_sc.items() if k > 0) / max(1, min(jobs, total))

    def remaining(self):
        return self.seconds({sc: k - self.done.get(sc, 0) for sc, k in self.planned.items()}, self.jobs)

    def describe(self):
        def rate(sc):
            r = sorted(self.rate.get(sc, []))
            return f", engine {r[len(r) // 2]:.2f} M msgs/s" if r else ""
        return "; ".join(f"{sc} {self.per_cell(sc):.1f} s/cell{rate(sc)}" for sc in SCENARIOS
                         if sc in self.planned and self.n.get(sc)) + (
               " (engine rates from these parallel, unpinned runs: not benchmark figures)" if self.rate else "")


def xml_safe(s):
    return XML_BAD.sub("�", s or "")


def write_junit(path, label, records, not_run, wall):
    fails = sum(r["outcome"] in ("FAIL", "INVALID") for r in records)
    errs = sum(r["outcome"] in ("CRASH", "TIMEOUT", "ERROR") for r in records)
    suite = ET.Element("testsuite", name=xml_safe(f"validate_seeds:{label}"), tests=str(len(records) + len(not_run)),
                       failures=str(fails), errors=str(errs), skipped=str(len(not_run)), time=f"{wall:.1f}")
    for r in sorted(records, key=lambda r: (r["seed"], SCENARIOS.index(r["scenario"]))):
        tc = ET.SubElement(suite, "testcase", classname=r["scenario"], name=f"seed {r['seed']}",
                           time=f"{r.get('wall_s', 0):.2f}")
        if r["outcome"] == "PASS":
            continue
        tag = "failure" if r["outcome"] in ("FAIL", "INVALID") else "error"
        msg = {"FAIL": f"report-stream hash {r.get('computed')} != published {r.get('expected')}",
               "INVALID": f"hash matches, but the harness verdict is {r.get('verdict')} (exit {r.get('returncode')}, "
                          f"probe share {r.get('probe_share')})",
               "CRASH": "the engine crashed (fatal signal)",
               "TIMEOUT": "the cell exceeded the time limit",
               "ERROR": f"no usable result (exit {r.get('returncode')})"}[r["outcome"]]
        el = ET.SubElement(tc, tag, message=xml_safe(msg), type=r["outcome"])
        el.text = xml_safe((r.get("detail", "") + "\nreproduce: " + r.get("repro", "")).strip())
    for seed, scenario in sorted(not_run, key=lambda c: (c[0], SCENARIOS.index(c[1]))):
        tc = ET.SubElement(suite, "testcase", classname=scenario, name=f"seed {seed}", time="0")
        ET.SubElement(tc, "skipped", message="not run (--fail-fast, or the run was stopped)")
    root = ET.Element("testsuites", name="seed validation", tests=suite.get("tests"), failures=suite.get("failures"),
                      errors=suite.get("errors"), skipped=suite.get("skipped"), time=suite.get("time"))
    root.append(suite)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def on_signal(signum, frame):
    raise KeyboardInterrupt(f"signal {signum}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("--engine", required=True, help="the engine adapter .so to validate")
    sel = ap.add_mutually_exclusive_group()
    sel.add_argument("--seeds", type=int, help="number of published seeds to draw at random (default: all)")
    sel.add_argument("--seed-list", help="explicit published seeds: a,b,c or @file (one per line, # comments)")
    ap.add_argument("--sample-seed", type=int, help="seed of the random draw and of the run order "
                    "(default: a fresh random value, printed)")
    ap.add_argument("--scenarios", default=",".join(SCENARIOS), help="comma-separated scenarios (default: all five)")
    ap.add_argument("--jobs", type=int, help="parallel harness runs (default: half the CPUs this process may use, "
                    f"capped by available memory at ~{GB_PER_JOB} GB per job)")
    ap.add_argument("--timeout", type=int, default=1200, help="seconds per cell, workload generation included "
                    "(default 1200; the harness's own watchdog allows 600 s per phase of a run)")
    ap.add_argument("--fail-fast", action="store_true", help="stop starting new cells after the first failure")
    ap.add_argument("--require-valid", action="store_true",
                    help="also require the harness's Verdict VALID and exit status 0. Off by default: the verdict "
                         "also applies the harness's anti-cheat time bounds, and on these deeper books an engine "
                         "whose engine_query_* calls are expensive (e.g. one that walks its book per query) can "
                         "exceed the probe-time bound and be marked INVALID with a matching hash, even on a quiet "
                         "machine; parallel runs make it more likely. The report-stream hash is the conformance "
                         "signal")
    ap.add_argument("--json", metavar="PATH", help="write a JSON report (selection, every cell, summary)")
    ap.add_argument("--junit", metavar="PATH", help="write a JUnit XML report (one test case per cell)")
    ap.add_argument("--keep-tapes", action="store_true", help="keep the generated workload files (~80 MB each)")
    ap.add_argument("--keep-results", action="store_true", help="keep the harness's per-run result JSON files")
    ap.add_argument("--dry-run", action="store_true", help="print the selection and exit")
    ap.add_argument("--estimate", action="store_true",
                    help="run a calibration sample (by default enough seeds to fill the jobs once, at least 2) and "
                         "print the projected time for the selection and for other sample sizes; does not run the "
                         "selection")
    ap.add_argument("--calibration-seeds", type=int, help="seeds in the --estimate sample (default: see --estimate)")
    args = ap.parse_args()

    scenarios = []
    for s in (x.strip() for x in args.scenarios.split(",")):
        if s and s not in scenarios:
            scenarios.append(s)
    bad = [s for s in scenarios if s not in SCENARIOS]
    if bad or not scenarios:
        die(f"unknown scenario(s) {bad}; choose from {','.join(SCENARIOS)}")
    for exe in (HARNESS, GENERATOR):
        if not os.access(exe, os.X_OK):
            die(f"missing {exe} - build the harness first (make)")
    engine = os.path.abspath(args.engine)
    if not os.path.isfile(engine):
        die(f"engine not found: {args.engine}")
    jobs = default_jobs() if args.jobs is None else args.jobs
    if jobs < 1:
        die("--jobs must be >= 1")
    if args.timeout < 1:
        die("--timeout must be >= 1")
    if args.calibration_seeds is not None and args.calibration_seeds < 1:
        die("--calibration-seeds must be >= 1")
    for opt, path in (("--json", args.json), ("--junit", args.junit)):
        if path and not os.access(os.path.dirname(os.path.abspath(path)), os.W_OK):
            die(f"{opt}: cannot write to {os.path.dirname(os.path.abspath(path))}")

    published, pool = load_published(scenarios)
    if not pool:
        die("no published seeds with hashes for every requested scenario")
    sample_seed = args.sample_seed if args.sample_seed is not None else secrets.randbits(32)
    rng = random.Random(sample_seed)
    if args.seed_list is not None:
        listed = parse_seed_list(args.seed_list)
        if CANONICAL_SEED in listed:
            die(f"--seed-list: seed {CANONICAL_SEED} is the canonical seed, not part of this set "
                "(scripts/run_challenge.py and ./harness check it directly)")
        missing = sorted(set(listed) - set(pool))
        if missing:
            die(f"--seed-list: not published for every requested scenario: {missing[:10]}"
                + (" ..." if len(missing) > 10 else ""))
        chosen = sorted(set(listed))
        how = f"{len(chosen)} listed seed(s)"
    elif args.seeds is not None:
        if not 1 <= args.seeds <= len(pool):
            die(f"--seeds must be between 1 and {len(pool)} (the published pool)")
        chosen = sorted(rng.sample(pool, args.seeds))
        how = f"{args.seeds} of {len(pool)} published seeds, drawn at random (--sample-seed {sample_seed})"
    else:
        chosen = pool
        how = f"all {len(pool)} published seeds"
    if not chosen:
        die("no seeds selected")
    cells = [(s, sc) for s in chosen for sc in scenarios]
    rng.shuffle(cells)                       # a representative early picture; --fail-fast stops sooner
    planned, calibration = cells, None
    if args.estimate:
        k = args.calibration_seeds or max(2, math.ceil(jobs / len(scenarios)))
        calibration = sorted(rng.sample(chosen, min(k, len(chosen))))
        cells = [(s, sc) for s in calibration for sc in scenarios]
        rng.shuffle(cells)

    print(f"engine:    {engine}")
    print(f"selection: {how}; scenarios {','.join(scenarios)}; {len(planned)} cells; {jobs} parallel job(s); "
          f"published pool {len(pool)} seeds; Python {sys.version.split()[0]}")
    again = [sys.argv[0], "--engine", args.engine, "--scenarios", ",".join(scenarios)]
    if args.seed_list is not None:
        again += ["--seed-list", args.seed_list]
    elif args.seeds is not None:
        again += ["--seeds", str(args.seeds)]
    again += ["--sample-seed", str(sample_seed)]       # also fixes the run order
    if args.estimate:
        again += ["--estimate", "--jobs", str(jobs)] + (["--calibration-seeds", str(args.calibration_seeds)]
                                                       if args.calibration_seeds else [])
    print(f"reproduce: {' '.join(shlex.quote(a) for a in again)}")
    if args.seeds is not None and len(chosen) <= 50:
        print(f"seeds:     --seed-list {','.join(map(str, chosen))}")
    if args.estimate:
        print(f"estimate:  calibration sample of {len(calibration)} seed(s) = {len(cells)} cells at up to "
              f"{min(jobs, len(cells))} parallel run(s) ({','.join(map(str, calibration))}); the selection itself "
              "is not run")
        if len(cells) < jobs:
            print(f"           note: fewer calibration cells than jobs, so the per-cell times are measured at "
                  f"{len(cells)} parallel runs; at {jobs} they can be higher")
    if args.dry_run:
        if not args.estimate and not (args.seeds is not None and len(chosen) <= 50):
            print("seeds:", " ".join(map(str, chosen)))
        return 0
    sys.stdout.flush()

    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)
    records, lock = [], threading.Lock()
    counts = {k: 0 for k in ("PASS", "FAIL", "CRASH", "TIMEOUT", "ERROR", "INVALID")}
    t0 = time.time(); last = [0.0]; interrupted = False
    model = CostModel(cells, jobs); announced = [False]

    def progress(force=False):
        now = time.time()
        if not announced[0] and model.calibrated() and len(records) >= min(jobs, len(cells)):
            announced[0] = True
            sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] measured {model.describe()}\n"
                             f"[{time.strftime('%H:%M:%S')}] projected: {fmt_dur(model.seconds(model.planned, jobs))}"
                             f" for all {len(cells)} cells with {jobs} job(s)\n")
        if not force and now - last[0] < 30:
            return
        last[0] = now
        done = len(records)
        if STOP.is_set():
            tail = "stopping: no new cells are started"
        else:
            eta = model.remaining() if done else None
            tail = f"ETA {fmt_dur(eta)}" + ("" if announced[0] else " (provisional)")
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] {done}/{len(cells)} cells, {counts['PASS']} pass, "
                         f"{done - counts['PASS']} not passed; elapsed {fmt_dur(now - t0)}, {tail}\n")

    def work(cell):
        if STOP.is_set():
            return None
        try:
            r = run_cell(engine, cell[0], cell[1], args, published)
        except Exception as e:                         # belt and braces: run_cell handles its own errors
            r = {"seed": cell[0], "scenario": cell[1], "outcome": "ERROR", "detail": f"validator error: {e!r}",
                 "wall_s": 0.0, "expected": published[cell]}
        if r["outcome"] == "NOT_RUN":
            return None
        with lock:
            records.append(r); counts[r["outcome"]] += 1; model.add(r)
            if r["outcome"] != "PASS":
                sys.stderr.write(f"  {r['outcome']}: seed {r['seed']} {r['scenario']}\n")
                if args.fail_fast:
                    STOP.set()
        return r

    ex = concurrent.futures.ThreadPoolExecutor(max_workers=jobs)
    futs = [ex.submit(work, c) for c in cells]
    pending = set(futs)
    try:
        while pending:
            done, pending = concurrent.futures.wait(pending, timeout=30,
                                                    return_when=concurrent.futures.FIRST_COMPLETED)
            with lock:
                progress()
            if STOP.is_set():
                for f in pending:
                    f.cancel()
    except KeyboardInterrupt:
        interrupted = True
        INTERRUPTED.set(); STOP.set()
        sys.stderr.write("stopping: killing the running cells (their files are removed)\n")
    for f in futs:
        f.cancel()
    while True:                                          # wait for the running cells; a second signal kills again
        try:
            if INTERRUPTED.is_set():
                kill_active()
            ex.shutdown(wait=True)
            break
        except KeyboardInterrupt:
            interrupted = True
            INTERRUPTED.set(); STOP.set()
    wall = time.time() - t0
    with lock:
        progress(force=True)

    ran = {(r["seed"], r["scenario"]) for r in records}
    not_run = [c for c in cells if c not in ran]
    ok = counts["PASS"] == len(cells) and not interrupted
    if args.estimate and records:
        by_sc = {}
        for _, sc in planned:
            by_sc[sc] = by_sc.get(sc, 0) + 1
        print(f"\nmeasured on {len(records)} calibration cells (up to {min(jobs, len(cells))} parallel): "
              f"{model.describe()}")
        print(f"projected for this selection ({len(planned)} cells, {jobs} job(s)): {fmt_dur(model.seconds(by_sc, jobs))}")
        sizes = sorted({n for n in (10, 100, 1000, len(pool)) if n <= len(pool)})
        print(f"projected by sample size (all requested scenarios), {jobs} job(s): " + ", ".join(
              f"{n:,} seeds {fmt_dur(model.seconds({sc: n for sc in scenarios}, jobs))}" for n in sizes))
    print(f"\n{'calibration ' if args.estimate else ''}result: {'PASS' if ok else 'NOT PASSED'} - "
          f"{counts['PASS']}/{len(cells)} cells passed"
          + "".join(f", {n} {k}" for k, n in counts.items() if k != "PASS" and n)
          + (f", {len(not_run)} not run" if not_run else "") + (" (stopped by a signal)" if interrupted else "")
          + f" ({fmt_dur(wall)})")
    fails = sorted((r for r in records if r["outcome"] != "PASS"), key=lambda r: (r["seed"], r["scenario"]))
    for r in fails[:50]:
        extra = (f" computed {str(r.get('computed'))[:16]}... expected {r['expected'][:16]}..."
                 if r["outcome"] == "FAIL" else "")
        print(f"  {r['outcome']:7s} seed {r['seed']} {r['scenario']}{extra}\n          reproduce: {r.get('repro', '')}")
    if len(fails) > 50:
        print(f"  ... and {len(fails) - 50} more (see --json / --junit)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"tool": "validate_seeds", "mode": "estimate" if args.estimate else "validate",
                       "engine": engine, "python": sys.version.split()[0], "published_pool": len(pool),
                       "selection": how, "sample_seed": sample_seed, "seeds": chosen,
                       "calibration_seeds": calibration, "scenarios": scenarios, "jobs": jobs,
                       "timeout_s": args.timeout, "require_valid": args.require_valid, "interrupted": interrupted,
                       "summary": {**counts, "cells": len(cells), "not_run": len(not_run), "wall_s": round(wall, 1),
                                   "passed": ok},
                       "cells": sorted(records, key=lambda r: (r["seed"], SCENARIOS.index(r["scenario"])))},
                      f, indent=1)
    if args.junit:
        write_junit(args.junit, os.path.basename(engine), records, not_run, wall)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
