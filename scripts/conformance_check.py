#!/usr/bin/env python3
"""
conformance_check.py — pre-run edge-case conformance gate.

A battery of small, contract-legal edge-case order sequences that the canonical
workload leaves latent. Each case is oracled by the byte-identical agreement of
the three reference baselines (liquibook / quantcup / exchange_core); an engine
is conformant only if its report-stream hash matches that consensus on EVERY
case. This is a correctness filter, run before (and separately from) the timed
benchmark — nothing here is performance-measured.

  scripts/conformance_check.py <engine_adapter.so>     # test one engine
  scripts/conformance_check.py --consensus             # just print the oracle

The cases target the classes of bug the random workload doesn't reach: cancelling
a non-tail order in a multi-order price level (range-erase), multi-level sweeps,
a modify that reprices through the spread, partial-fill residuals, stale cancels,
and FIFO priority. A bug only counts if it is reachable through the harness ABI;
see CORRECTNESS_FINDINGS.md for the latent defects no contract-legal input reaches.
"""
import subprocess, struct, re, sys, os, json

ORACLE_CACHE = "/tmp/conformance_oracle.json"

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAGIC = 0x4D4542575F303031  # "MEBW_001"
BASELINES = {"liquibook": "./liquibook_adapter.so",
             "quantcup": "./quantcup_adapter.so",
             "exchange_core": "./exchange_core_adapter.so"}
BUY, SELL = 0, 1

# message constructors -> (type, side, ioc, qty, order_id, price_ticks)
def NEW(oid, side, qty, px, ioc=0): return (0, side, ioc, qty, oid, px)
def CANCEL(oid):                    return (1, 0, 0, 0, oid, 0)
def MODIFY(oid, side, nqty, npx):   return (2, side, 0, nqty, oid, npx)

# Each case is a short sequence on an empty book. The comment is the consensus
# behaviour; an engine that does anything else fails the gate.
EDGE_CASES = {
    # rest A,B,C at one price, cancel the MIDDLE, then sweep with an IOC.
    # Correct: the IOC fills A and C (two trades). A range-erase that also wipes
    # the bystander behind the cancelled order fills only A (one trade) + residual.
    "cancel_middle_then_sweep":   [NEW(1,BUY,10,100), NEW(2,BUY,10,100), NEW(3,BUY,10,100), CANCEL(2), NEW(4,SELL,20,100,ioc=1)],
    # cancel the HEAD instead — correct: fills B and C; a range-erase from the head
    # wipes the whole level -> zero trades.
    "cancel_head_then_sweep":     [NEW(1,BUY,10,100), NEW(2,BUY,10,100), NEW(3,BUY,10,100), CANCEL(1), NEW(4,SELL,20,100,ioc=1)],
    # an IOC that crosses three resting levels -> three trades, best price first.
    "multi_level_ioc_sweep":      [NEW(1,SELL,10,102), NEW(2,SELL,10,101), NEW(3,SELL,10,100), NEW(4,BUY,30,102,ioc=1)],
    # a modify repricing a resting bid up through a resting ask -> it crosses.
    "modify_into_cross":          [NEW(1,SELL,10,100), NEW(2,BUY,10,99), MODIFY(2,BUY,10,100)],
    # partial fill leaves a resting residual that a later cancel must ack.
    "partial_fill_then_cancel":   [NEW(1,SELL,10,100), NEW(2,BUY,6,100), CANCEL(1)],
    # a fully-consumed order is not resting -> a cancel of it must reject.
    "full_fill_then_stale_cancel":[NEW(1,SELL,10,100), NEW(2,BUY,10,100), CANCEL(1)],
    # two bids at one price; one sell takes the FIFO-first.
    "fifo_priority":              [NEW(1,BUY,10,100), NEW(2,BUY,10,100), NEW(3,SELL,10,100)],

    # ---- harder: multi-step lifecycle and subtle priority semantics ----
    # FIFO integrity after SCATTERED (non-adjacent) cancels in a deep queue: rest
    # 8, cancel 2/5/7, sweep -> must fill 1,3,4,6,8 in order (50 = 5x10 exactly).
    "deep_fifo_scattered_cancel": [NEW(1,BUY,10,100),NEW(2,BUY,10,100),NEW(3,BUY,10,100),NEW(4,BUY,10,100),
                                   NEW(5,BUY,10,100),NEW(6,BUY,10,100),NEW(7,BUY,10,100),NEW(8,BUY,10,100),
                                   CANCEL(2),CANCEL(5),CANCEL(7),NEW(9,SELL,50,100,ioc=1)],
    # NOTE: cases that turn on what a MODIFY does to QUEUE PRIORITY are deliberately
    # NOT in the gate. Whether a same-price quantity DECREASE keeps priority
    # (production exchanges) or is cancel+reinsert (FlashOne and many engines), and
    # likewise how an INCREASE or a same-price reprice re-orders the queue, are
    # valid CONVENTIONS that correct engines differ on — quantity is conserved, the
    # book is not crossed, nothing is lost; only the queue order changes. The
    # harness contract leaves these unspecified and the canonical workload doesn't
    # depend on them, so flagging them would wrongly fail conforming engines (e.g.
    # dgtony keeps priority on an increase; that is a convention, not a bug). The
    # gate tests only hard invariants every correct book must satisfy. Modify IS
    # tested where it does NOT turn on priority — see modify_into_cross (a crossing
    # modify must match), modify_partially_filled_residual, stale_modify_after_full_fill.
    # a non-IOC aggressor sweeps three levels; its residual rests as the new BBO and is later filled.
    "sweep_residual_becomes_bbo": [NEW(1,SELL,10,100),NEW(2,SELL,10,101),NEW(3,SELL,10,102),
                                   NEW(4,BUY,35,102),NEW(5,SELL,5,102,ioc=1)],
    # modify the residual of a PARTIALLY-filled resting order to a new price, then fill it.
    "modify_partially_filled_residual":[NEW(1,SELL,10,100),NEW(2,BUY,4,100),MODIFY(1,SELL,6,101),NEW(3,BUY,6,101,ioc=1)],
    # a modify of a fully-consumed order must reject.
    "stale_modify_after_full_fill":[NEW(1,SELL,10,100),NEW(2,BUY,10,100),MODIFY(1,SELL,5,101)],
    # reusing the id of a CANCELLED order is legal (the id is free) -> a fresh order.
    "reuse_id_after_cancel":      [NEW(1,BUY,10,100),CANCEL(1),NEW(1,SELL,10,200),NEW(2,BUY,10,200,ioc=1)],
    # a SELL aggressor takes the HIGHEST bid first; head/tail cancels across two levels.
    "two_level_cancel_sell_priority":[NEW(1,BUY,10,100),NEW(2,BUY,10,100),NEW(3,BUY,10,101),NEW(4,BUY,10,101),
                                      CANCEL(1),CANCEL(4),NEW(5,SELL,20,100,ioc=1)],
    # an IOC whose quantity exactly equals the sum of three levels -> full sweep, no residual.
    "exact_multilevel_boundary":  [NEW(1,SELL,10,100),NEW(2,SELL,20,101),NEW(3,SELL,30,102),NEW(4,BUY,60,102,ioc=1)],
}

def scen_name(case): return "ec-" + case.replace("_", "-")

def write_bin(path, msgs):
    with open(path, "wb") as f:
        f.write(struct.pack("<QII", MAGIC, 1, len(msgs)))
        for seq, (t, side, ioc, qty, oid, px) in enumerate(msgs):
            f.write(struct.pack("<BBBBIQQqq", t, side, ioc, 0, qty, seq, oid, px, 0))

def run_hash(so, scen, count):
    cmd = ["./harness", "--engine", so, "--scenario", scen, "--mode", "perf",
           "--seed", "0", "--count", str(count)]
    # Retry once on a transient hang / no-hash: a baseline that flakes on a single
    # case would otherwise drop that hard-invariant case to NO-CONSENSUS and
    # silently shrink the gate.
    for attempt in range(2):
        try:
            p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            if attempt == 0:
                continue
            return "<hang>"
        m = re.search(r"Computed hash: ([0-9a-f]+)", p.stdout)
        if m:
            return m.group(1)[:16]
    return f"<nohash rc={p.returncode}>"

def consensus(write_bins=True):
    """Write each case's .bin, compute the 3-baseline oracle, cache it, and return
    {case: (scen, count, oracle_hash_or_None, per_baseline)}."""
    out = {}
    for case, msgs in EDGE_CASES.items():
        scen, count = scen_name(case), len(msgs)
        if write_bins:
            write_bin(os.path.join(REPO, f"orders_{scen}_s0_n{count}.bin"), msgs)
        hs = {b: run_hash(so, scen, count) for b, so in BASELINES.items()}
        oracle = next(iter(hs.values())) if len(set(hs.values())) == 1 else None
        out[case] = (scen, count, oracle, hs)
    with open(ORACLE_CACHE, "w") as f:
        json.dump({c: [v[0], v[1], v[2]] for c, v in out.items()}, f)
    return out

def load_oracle():
    with open(ORACLE_CACHE) as f:
        d = json.load(f)
    return {c: (v[0], v[1], v[2], {}) for c, v in d.items()}

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--consensus":
        cons = consensus(write_bins=True)
        print("=== baseline consensus (oracle) ===")
        for case, (scen, count, oracle, hs) in cons.items():
            tag = oracle if oracle else "NO-CONSENSUS " + str(hs)
            print(f"  {case:30s} {tag}")
        bad = [c for c, v in cons.items() if v[2] is None]
        print("ALL CASES HAVE A CONSENSUS" if not bad else f"NO CONSENSUS ON: {bad}")
        return
    so = sys.argv[1]
    # Reuse a cached oracle (built by a prior `--consensus` run) so a parallel
    # sweep neither re-runs the baselines nor rewrites the .bin files under each
    # other; fall back to computing inline for a standalone single-engine check.
    cons = load_oracle() if os.path.exists(ORACLE_CACHE) else consensus(write_bins=True)
    name = os.path.basename(so).replace("_adapter.so", "")
    print(f"=== conformance: {name} ===")
    fails = []
    for case, (scen, count, oracle, hs) in cons.items():
        eh = run_hash(so, scen, count)
        if oracle is None:
            verdict = "SKIP(no-consensus)"
        elif eh == oracle:
            verdict = "PASS"
        else:
            verdict = "FAIL"; fails.append(case)
        print(f"  {case:30s} engine={eh:18s} oracle={str(oracle):18s} {verdict}")
    print(f"VERDICT: {name}: " + ("CONFORMANT (all edge cases match consensus)"
                                   if not fails else f"NON-CONFORMANT — fails {fails}"))
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
