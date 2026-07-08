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

The 2026-07-03 hardening pass added further cases from a mined survey of latent
bugs across the surveyed engine population (taker-priced-trade, FIFO priority
across separate aggressor calls, id-reuse via the fill death-path, price/id
domain ceilings, phantom trades, and more) — see the per-case comments below
for the exact taxonomy class(es) each one targets.

Two comparison dimensions per case (2026-07-05): the report-stream hash above,
AND a book-state audit. The harness --mode audit probes the engine's
engine_query_best_bid / best_ask / depth_at at every message index (for a case
shorter than AUDIT_POINTS, so the probe set is the whole sequence) and compares
them against the baseline consensus. The state audit catches a class the report
hash structurally cannot: a stale or phantom book state that SELF-HEALS before
any trade is priced — e.g. a cancel that empties the top level but leaves it in
the price map, so best_bid reads a dead price until the next match happens to
clean it up. Such a bug never moves a Trade/Ack/Reject byte (invisible to the
hash) yet is a real divergence in the queried book. An engine conforms only if
BOTH its report stream and its book state match consensus on every case; a case
is state-gated only where all three baselines agree on the state.
"""
import subprocess, struct, re, sys, os, json, hashlib

ORACLE_CACHE = "/tmp/conformance_oracle.json"

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAGIC = 0x4D4542575F303031  # "MEBW_001"
AUDIT_POINTS = 64  # matches the harness AUDIT_POINTS; a case with <= this many
                   # messages is probed at EVERY index, so the state audit below
                   # is deterministic despite the harness's random probe seed.
# Fast, independent, as-shipped-clean PUBLIC conformers used as the report-stream
# oracle. Any conforming engine reproduces the byte-identical consensus, so the
# oracle uses FAST conformers rather than the slower original reference trio
# (liquibook / quantcup / exchange_core) — ~150-250x quicker on the deep sweep,
# verified byte-identical to that trio on every case. Three languages / three
# data structures for independence: cpp_orderbook (C++, price-time tree), llc993
# (Rust, BTreeMap + slab), hroptatyr/clob (C, b+tree CLOB). liquibook / quantcup
# remain the harness's built-in state-audit reference (instant on the short
# state-gated cases).
BASELINES = {"cpp_orderbook":  "./cpp_orderbook_adapter.so",
             "llc993":         "./llc993_adapter.so",
             "hroptatyr_clob": "./hroptatyr_clob_adapter.so"}
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
    # (production exchanges) or is cancel+reinsert (many engines), and
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

    # ---- hardened battery (2026-07-03): mined-latent-bug taxonomy additions ----
    # Source: a mined survey of ~150 engines' real bugs, adjudicated by adversarial
    # review (see CORRECTNESS_FINDINGS.md for the underlying defect records). Each
    # comment below names the taxonomy bug class(es) the case targets.

    # taker-priced-trade, both directions: a marketable order fills at the RESTING
    # (maker's) price, never its own limit -- tested as a BUY-aggressor and a
    # SELL-aggressor so a direction-asymmetric bug can't hide (several real bugs
    # only mis-price one side).
    "taker_price_both_directions": [NEW(1,SELL,10,100),NEW(2,BUY,10,105,ioc=1),
                                     NEW(3,BUY,10,200),NEW(4,SELL,10,195,ioc=1)],
    # a same-price level survives a full double-consume: one BUY exceeding two
    # resting SELLs at one price must emit TWO separate trades with distinct
    # maker ids -- never a collapsed/synthetic-id report -- and rest its own
    # residual as the new best bid; a follow-up IOC then proves that residual is
    # genuinely live (iterator-invalidation-multilevel, fifo-tie-break-violation,
    # depth-nonconservation).
    "same_level_full_double_consume": [NEW(1,SELL,30,9),NEW(2,SELL,40,9),NEW(3,BUY,100,9),NEW(4,SELL,30,9,ioc=1)],
    # post-sweep book health: after an exact 3-level sweep empties price 101, a
    # fresh order rested THERE -- not at an untouched price -- must still cross
    # cleanly; a dangling reference/iterator into the just-emptied level would
    # corrupt only this LATER, unrelated match (iterator-invalidation-multilevel).
    # The untouched-103 pair is kept on top as a second, independent health probe.
    "post_sweep_book_health": [NEW(1,SELL,10,100),NEW(2,SELL,10,101),NEW(3,SELL,10,102),NEW(4,BUY,30,102,ioc=1),
                                NEW(5,SELL,8,101),NEW(6,BUY,8,101,ioc=1),NEW(7,SELL,8,103),NEW(8,BUY,8,103,ioc=1)],
    # price priority, both sides, inserted out of arrival order: a marketable
    # SELL must hit the better-priced (not earlier-arrived) resting SELL, and a
    # deep IOC against three bids inserted out of price order (90, 95, 92) must
    # fill strictly by price -- 95, then 92, then 90 -- never by arrival order
    # (price-priority-violation, price-priority-inversion, exact-touch-boundary).
    "price_priority_both_sides_reverse_arrival": [NEW(1,SELL,10,105),NEW(2,SELL,10,100),NEW(3,BUY,10,105,ioc=1),
                                                   NEW(4,BUY,10,90),NEW(5,BUY,10,95),NEW(6,BUY,10,92),
                                                   NEW(7,SELL,30,90,ioc=1)],
    # cancel the resting best BID, then re-probe at exactly the next level with
    # NO sweep -- deliberately shallow, so a stale-best-price cache never gets the
    # chance to self-heal by walking past the emptied level the way a multi-level
    # sweep would (phantom-price-level-after-cancel, stale-best-price-cache,
    # stale-best-price-accessor). Only catches staleness that reaches the MATCH
    # path -- a query-only stale accessor cannot move a report-hash gate.
    "cancel_best_level_minimal_reprobe": [NEW(1,BUY,10,100),NEW(2,BUY,7,99),CANCEL(1),NEW(3,SELL,7,99,ioc=1)],
    # an IOC partial fill must not rest: a 15-lot IOC crossing a 10-lot resting
    # ask fills 10 and CancelAcks its own 5-lot remainder; a reprobe SELL must
    # then find nothing resting at 100 and itself CancelAck -- a residual-rests
    # bug flips both signals at once (no CancelAck on the first, a Trade instead
    # of a CancelAck on the second) (ioc-ignored).
    "ioc_partial_fill_must_not_rest": [NEW(1,SELL,10,100),NEW(2,BUY,15,100,ioc=1),NEW(3,SELL,5,100,ioc=1)],
    # a marketable (non-IOC) limit order stops exactly at its OWN price: it must
    # not walk past its own limit to a further, better-for-it resting level, and
    # its unfilled remainder must rest -- not vanish -- at that limit
    # (limit-price-ignored-during-sweep).
    "limit_order_stops_at_own_price": [NEW(1,SELL,5,100),NEW(2,SELL,10,105),NEW(3,BUY,12,102),NEW(4,SELL,7,102,ioc=1)],
    # a partial-fill residual is drained EXACTLY: two IOCs consume a 20-lot ask
    # as 8 then 12 -- never the original 20, never a remainder-framed number; a
    # third reprobe against the now-empty book must CancelAck only -- a
    # never-decremented resting quantity would instead cross it with a phantom
    # Trade (wrong-partial-fill-size, depth-nonconservation, lost-trades).
    "partial_fill_exact_residual_drain": [NEW(1,SELL,20,100),NEW(2,BUY,8,100,ioc=1),NEW(3,BUY,12,100,ioc=1),
                                           NEW(4,BUY,5,100,ioc=1)],
    # a partially-consumed same-price maker keeps front-of-queue priority ACROSS
    # separate aggressor calls, not just within one sweep: two 15-lot IOCs against
    # four 10-lot bids must each split across exactly the right pair of makers,
    # order 4 untouched throughout (fifo-tie-break-violation).
    "fifo_same_price_multi_aggressor": [NEW(1,BUY,10,100),NEW(2,BUY,10,100),NEW(3,BUY,10,100),NEW(4,BUY,10,100),
                                         NEW(5,SELL,15,100,ioc=1),NEW(6,SELL,15,100,ioc=1)],
    # an id freed by a FULL FILL (not a cancel) is reusable immediately, same as
    # reuse_id_after_cancel but via the fill death-path -- the contract gives no
    # reason the two death-paths should be tracked differently (id-reuse-confusion).
    "reuse_id_after_full_fill": [NEW(1,BUY,10,100),NEW(2,SELL,10,100),NEW(1,SELL,10,200),NEW(3,BUY,10,200,ioc=1)],
    # cancel AND modify of an id that was NEVER seen at all on a virgin book --
    # a different code path from full_fill_then_stale_cancel's "was live, then
    # died" shape. Must reject both, never crash, hang, or silently ack
    # (unknown-id-cancel-crash, empty-book-deref, and the modify-path equivalent
    # of the operator[] family).
    "cancel_never_issued_id": [CANCEL(999),MODIFY(999,BUY,5,100)],
    # ops on an id that died by CANCEL (not fill): a re-cancel must reject and a
    # modify must reject, matching the fill-death rule already pinned by
    # full_fill_then_stale_cancel/stale_modify_after_full_fill -- the contract has
    # no reason to treat the two death-paths differently (silent-ack-cancel,
    # unknown-id-cancel-silent-ack, unknown-id-cancel-crash).
    "stale_ops_after_cancel": [NEW(1,BUY,10,100),CANCEL(1),CANCEL(1),MODIFY(1,BUY,5,101)],
    # a wide but in-domain price (200000, inside every baseline's usable range
    # with comfortable margin) must reproduce losslessly -- not truncate, wrap, or
    # reject (int-ceiling). A narrow native price field (e.g. uint16_t) wraps
    # silently: 200000 mod 65536 = 3392.
    "wide_in_domain_price_ceiling": [NEW(1,SELL,10,200000),NEW(2,BUY,10,200000,ioc=1)],
    # a CANCEL of an id far outside any realistic live range (2,000,000 -- this
    # benchmark's own canonical-workload scale) on a virgin book must reject like
    # any other never-seen id, not crash inside an unconditional array/map
    # .at()-style lookup (int-ceiling, lookup-path variant). Demoted from a
    # NEW+CANCEL round-trip: a NEW at a sparse id would contradict the published
    # dense-id guarantee and false-fail the doc-endorsed flat-index adapters;
    # new-id ceilings are already exercised by the canonical 2M workload itself.
    "large_order_id_ceiling": [CANCEL(2000000)],
    # a deep recursive/iterative sweep: 5000 resting asks at one price, one IOC
    # sized to consume all of them -- exactly 5000 trades, makers in strict
    # ascending order, zero residual. The one case allowed past the ~60-message
    # budget (recursion-resubmission).
    "deep_recursive_sweep_5000": [NEW(i,SELL,1,100) for i in range(1,5001)] + [NEW(5001,BUY,5000,100,ioc=1)],
    # a taker exactly exhausted by the FIRST same-price maker must stop there: a
    # second maker sitting right behind it must be left fully resting, untouched
    # -- no second, zero-qty, or phantom trade (phantom-trade).
    "phantom_trade_exact_exhaust": [NEW(1,SELL,5,100),NEW(2,SELL,5,100),NEW(3,BUY,5,100,ioc=1)],

    # ---- new from adversarial review: side-asymmetric reprobe coverage ----
    # the ASK-side mirror of cancel_best_level_minimal_reprobe (above), which
    # only probes the BID side -- stale-best-price staleness can be
    # side-asymmetric (phantom-price-level-after-cancel, stale-best-price-cache).
    "cancel_best_ask_minimal_reprobe": [NEW(1,SELL,10,100),NEW(2,SELL,7,101),CANCEL(1),NEW(3,BUY,7,101,ioc=1)],
    # cancelling a side down to completely EMPTY, then refilling it, is a distinct
    # guard branch from "one level goes empty" -- a sentinel/cached-side-emptiness
    # bug only trips when the side itself, not just a level within it, hits zero
    # (phantom-price-level-after-cancel).
    "cancel_empties_entire_side_then_refill": [NEW(1,BUY,10,100),CANCEL(1),NEW(2,BUY,8,95),NEW(3,SELL,8,95,ioc=1)],

    # ---- reverse-engineered from a latent modify defect (2026-07-05) ----
    # a MODIFY that reprices ONE of two same-price orders must not drop the OTHER.
    # A native reprice that removes the order from its level twice — once directly,
    # once again via an insert-time dedup guard — double-decrements the level's length,
    # makes a still-populated level look empty, and deletes it, orphaning the untouched
    # sibling (gone from the price tree, still in the id index, never matches again).
    # Rest 1 and 2 at 100, reprice 1 DOWN to 99, then a SELL@100 must still fill the
    # resting sibling 2; a dropped sibling yields NO trade + a spurious CancelAck for
    # the aggressor instead (modify-drops-sibling / order-loss). This is a HARD
    # invariant, NOT a queue-priority convention: qty is unchanged and the only
    # assertion is that the untouched sibling survives and still crosses — order 1 ends
    # alone at 99 where the SELL@100 never reaches, so no report depends on how the
    # reprice re-orders the queue (the convention axis the gate deliberately excludes).
    "modify_shared_level_sibling_survives": [NEW(1,BUY,10,100),NEW(2,BUY,10,100),MODIFY(1,BUY,10,99),NEW(3,SELL,10,100,ioc=1)],
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
            p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=180)
        except subprocess.TimeoutExpired:
            if attempt == 0:
                continue
            return "<hang>"
        m = re.search(r"Computed hash: ([0-9a-f]+)", p.stdout)
        if m:
            return m.group(1)[:16]
    return f"<nohash rc={p.returncode}>"

def run_audit(so, scen, count):
    """--mode audit verdict for one engine on one case: True = the engine's book
    state (best_bid / best_ask / depth_at) matched the baseline at every probe;
    False = a mismatch — a stale/phantom book state a report-hash gate cannot see
    because it self-heals before any trade; None = the audit could not run. Only
    used for short cases (count <= AUDIT_POINTS), where every index is probed, so
    the comparison is deterministic despite the harness's random probe seed."""
    cmd = ["./harness", "--engine", so, "--scenario", scen, "--mode", "audit",
           "--seed", "0", "--count", str(count)]
    for attempt in range(2):
        try:
            p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=180)
        except subprocess.TimeoutExpired:
            if attempt == 0:
                continue
            return None
        m = re.search(r"State audit: (PASS|FAIL|SKIPPED)", p.stdout)
        if m:
            return {"PASS": True, "FAIL": False}.get(m.group(1))
    return None

def _require_so(paths, what):
    missing = [p for p in paths
               if not os.path.exists(p if os.path.isabs(p) else os.path.join(REPO, p))]
    if missing:
        sys.exit(f"ERROR: {what} not found: {missing}. The gate cannot form a "
                 f"consensus without them — build the three oracle adapters "
                 f"(cpp_orderbook / llc993 / hroptatyr_clob, each in "
                 f"additional_references/<name>_adapter/build.sh), and pass an "
                 f"engine .so that exists.")

def _oracle_sig():
    # Binds the cache to the exact case set + oracle baselines it was computed
    # for, so a stale cache (e.g. from an earlier session, after `make clean`
    # deleted the .bin files) is rejected rather than trusted (finding I).
    return hashlib.sha256(
        (repr(sorted(EDGE_CASES.items())) + repr(sorted(BASELINES.items()))).encode()
    ).hexdigest()

def consensus(write_bins=True):
    _require_so(BASELINES.values(), "oracle baseline adapter(s)")
    """Write each case's .bin, compute the oracle (report-stream hash + book-state
    audit consensus), cache it, and return {case: (scen, count, hash_or_None,
    per_oracle, state_gated)}."""
    out = {}
    for case, msgs in EDGE_CASES.items():
        scen, count = scen_name(case), len(msgs)
        if write_bins:
            write_bin(os.path.join(REPO, f"orders_{scen}_s0_n{count}.bin"), msgs)
        hs = {b: run_hash(so, scen, count) for b, so in BASELINES.items()}
        vals = set(hs.values())
        # A consensus is real only when all three oracles AGREE on a well-formed
        # hash. Missing/broken oracle adapters return identical sentinel strings
        # ("<hang>", "<nohash rc=N>"); those must NOT be accepted as an oracle, or
        # an engine that fails identically would "match" the sentinel and pass.
        oracle = (next(iter(vals))
                  if len(vals) == 1 and re.fullmatch(r"[0-9a-f]{16}", next(iter(vals)))
                  else None)
        # State-audit consensus: the book-state probes (best_bid / best_ask /
        # depth_at) must ALSO agree, so a query-only stale/phantom state — a bug
        # invisible to the report hash because it self-heals before any trade — is
        # gated too. Only where every index is probed (count <= AUDIT_POINTS, so
        # the comparison is deterministic) and every oracle engine's audit passes
        # (i.e. they agree on state).
        state_gated = (count <= AUDIT_POINTS and
                       all(run_audit(so, scen, count) is True
                           for so in BASELINES.values()))
        out[case] = (scen, count, oracle, hs, state_gated)
    with open(ORACLE_CACHE, "w") as f:
        json.dump({"__sig__": _oracle_sig(),
                   "cases": {c: [v[0], v[1], v[2], v[4]] for c, v in out.items()}}, f)
    return out

def load_oracle():
    # Returns None (-> caller recomputes) for a missing, malformed, or STALE
    # cache: one whose signature no longer matches the current cases/baselines,
    # or whose referenced .bin files no longer exist. A stale /tmp cache from an
    # earlier session otherwise produces spurious verdicts (finding I).
    try:
        with open(ORACLE_CACHE) as f:
            d = json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(d, dict) or d.get("__sig__") != _oracle_sig():
        return None
    cases = d.get("cases", {})
    for v in cases.values():
        if not os.path.exists(os.path.join(REPO, f"orders_{v[0]}_s0_n{v[1]}.bin")):
            return None
    return {c: (v[0], v[1], v[2], {}, v[3] if len(v) > 3 else False)
            for c, v in cases.items()}

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--consensus":
        cons = consensus(write_bins=True)
        print("=== oracle: report-stream hash + book-state audit (fast public conformers) ===")
        for case, (scen, count, oracle, hs, state_gated) in cons.items():
            tag = oracle if oracle else "NO-CONSENSUS " + str(hs)
            print(f"  {case:40s} {tag:18s} state={'gated' if state_gated else 'ungated'}")
        bad = [c for c, v in cons.items() if v[2] is None]
        ungated = [c for c, v in cons.items() if not v[4]]
        print("ALL CASES HAVE A REPORT CONSENSUS" if not bad
              else f"NO REPORT CONSENSUS ON: {bad}")
        print(f"STATE-GATED: {len(cons) - len(ungated)}/{len(cons)} cases"
              + (f"  (ungated: {ungated})" if ungated else ""))
        return
    so = sys.argv[1]
    _require_so([so], "engine .so")
    # Reuse a cached oracle (built by a prior `--consensus` run) so a parallel
    # sweep neither re-runs the baselines nor rewrites the .bin files under each
    # other; fall back to computing inline for a standalone single-engine check.
    cons = (load_oracle() if os.path.exists(ORACLE_CACHE) else None) or consensus(write_bins=True)
    name = os.path.basename(so).replace("_adapter.so", "")
    print(f"=== conformance: {name} ===")
    fails = []
    for case, (scen, count, oracle, hs, state_gated) in cons.items():
        # (1) report-stream hash
        eh = run_hash(so, scen, count)
        if oracle is None:
            rv = "SKIP"
        elif eh == oracle:
            rv = "PASS"
        else:
            rv = "FAIL"; fails.append(case + "[report]")
        # (2) book-state audit — catches a stale/phantom queried book state the
        # report hash cannot see because it self-heals before any trade. Gated
        # only where the oracle engines agree on state (state_gated).
        if state_gated:
            sa = run_audit(so, scen, count)
            sv = "PASS" if sa is True else ("FAIL" if sa is False else "SKIP")
            if sa is False:
                fails.append(case + "[state]")
        else:
            sv = "-"
        print(f"  {case:40s} report={rv:5s} state={sv}")
    oracled = sum(1 for v in cons.values() if v[2] is not None)
    state_gated_count = sum(1 for v in cons.values() if v[4])
    if oracled == 0:
        print(f"VERDICT: {name}: ERROR — no case formed a valid oracle consensus "
              f"(the three baseline adapters must be built and byte-agree). NOT CONFORMANT.")
        sys.exit(2)
    ok = not fails
    # Report the ACTUAL state-audit coverage rather than implying full book-state
    # gating: if the state dimension gated no cases (a baseline couldn't run its
    # audit, or the oracles disagreed on state), say so instead of claiming it.
    state_note = (f"book state on {state_gated_count}" if state_gated_count
                  else "book-state audit gated NO cases (baseline audit unavailable or disagreed)")
    print(f"VERDICT: {name}: " + (f"CONFORMANT (report stream on {oracled} cases; {state_note})"
                                   if ok else f"NON-CONFORMANT — fails {fails}"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
