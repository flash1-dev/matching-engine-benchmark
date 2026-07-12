# The conformance gate

`scripts/conformance_check.py` is a **pre-run correctness filter**, run before and
separately from the timed benchmark. An engine is listed conforming only if it
passes this gate *and* reproduces the byte-identical consensus on the canonical
workload (across 100 seeds, with the 192-point state audit). Nothing here is
performance-measured.

## Why a separate gate

The canonical workload is a realistic, random order flow calibrated to a liquid
equity. That realism is the point for *throughput* — but it means whole classes
of correctness bug could stay **latent**, because the random flow — based on a GBM
price process — is limited in its ability to construct the exact shapes that trigger
them. Several real defects we found live exactly there.

Shoe-horning such a case into the timed workload would be wrong: it pollutes a
realistic order flow with a synthetic bug-trap and distorts the throughput
number. So the edge cases live in a dedicated battery instead — short,
contract-legal message sequences run outside the timed path.

## What it tests

Each case is a handful of messages on an empty book, oracled by the
**byte-identical agreement of independent conforming engines**. The oracle was
first anchored by three of them (Liquibook, QuantCup, Exchange-core); the gate now
computes it from three **fast** public conformers — cpp_orderbook (C++), llc993
(Rust), and hroptatyr/clob (C) — which reproduce those exact hashes but run the
battery ~40× quicker (the deep-sweep case alone drops from minutes to seconds).
Any conforming engine yields the identical oracle, so the swap changes nothing but
the wall-clock. An engine conforms only if, on **every** case, BOTH signals match
the consensus: (1) its **report-stream hash**, and (2) its **book-state audit** —
the `engine_query_best_bid` / `best_ask` / `depth_at` answers it gives at each
probe (see *The book-state audit* below).

The battery is **34 hand-crafted cases**, reverse-engineered from latent bugs found
across the surveyed field and each validated to the unanimous consensus before
admission. The battery grows as new latent-bug classes surface; a per-engine integration record cites the case count current when that engine was gated. Every case is a **hard invariant** — a property every correct
price-time-priority book must satisfy regardless of implementation — and each runs
in its own subprocess under a 180 s timeout, so a slow-but-correct engine is never
failed for time. The invariants probed (see the source for exact sequences):

| Probe | Invariant |
|:-----|:----------|
| `cancel_middle_then_sweep`, `cancel_head_then_sweep` | cancelling a non-tail order in a same-price FIFO must not lose the orders behind it |
| `deep_fifo_scattered_cancel` | FIFO integrity after several non-adjacent cancels in a deep queue |
| `multi_level_ioc_sweep`, `exact_multilevel_boundary` | a marketable order fills across price levels best-price-first, exact-fill leaves nothing resting |
| `sweep_residual_becomes_bbo` | a non-IOC aggressor's residual rests as the new touch and is then fillable |
| `modify_into_cross` | a modify that reprices through the spread must match, not rest crossed |
| `modify_into_cross_price_improvement` | a crossing modify whose new limit is strictly *better* than the resting maker must still fill at the **maker's** price, both directions — the modify-path sibling of the taker-priced-trade rule (a fill at the modify's own more-aggressive limit is invisible when the reprice lands exactly on the maker, so this case reprices past it) |
| `modify_shared_level_sibling_survives` | repricing one of two same-price orders must not lose the untouched sibling (a reprice that double-decrements the shared level orphans it) |
| `partial_fill_then_cancel`, `modify_partially_filled_residual` | a partial-fill residual is conserved and remains cancel/modify-able |
| `full_fill_then_stale_cancel`, `stale_modify_after_full_fill` | a cancel/modify of a fully-filled (non-resting) order must be **rejected**, not acked |
| `fifo_priority`, `two_level_cancel_sell_priority` | price priority across levels, FIFO time priority within a level |
| `reuse_id_after_cancel` | the id of a cancelled order is free and reusable |
| taker-priced trades, both directions | a buy lifting a cheaper ask and a sell hitting a higher bid must each print at the resting maker's price (several real engines mis-price only one side) |
| phantom price level | cancelling the best level, with no follow-on sweep to let a stale cache self-heal, must not leave a dead best price visible |
| IOC residual | a partially-filled IOC cancels its remainder instead of joining the book |
| marketable limit stops at its own price | a mid-sweep marketable limit rests its remainder rather than walking through its own limit to a further level |
| id reuse after a full fill | an id freed by a fill (not a cancel) is immediately reusable |
| never-issued id | a cancel or modify of an id never issued on a virgin book is rejected |
| deep same-price FIFO across calls | a partially-consumed maker keeps front-of-queue priority between separate aggressor calls |
| wide in-domain price | a price near the representational ceiling is handled in full |
| deep 5,000-fill sweep | a single-price 5,000-fill sweep completes correctly (report-hash gated) |

## The book-state audit

Beyond the report stream, the gate compares **book state**: after each case it probes
the engine's `engine_query_best_bid` / `best_ask` / `depth_at` — at every message
index on these short sequences — against the same consensus. This catches a class the
report hash structurally cannot: a **stale or phantom book state that self-heals
before any trade is priced**. The canonical example is a cancel that empties the top
level but leaves it in the price map, so `best_bid` returns a dead price until the
next match happens to sweep it: the report stream never diverges (no wrong trade is
printed) yet the queried book is momentarily wrong, and a direct BBO/depth query at
that instant sees it. (A case is state-gated only where the oracle conformers agree
on state; the single 5,001-message deep-sweep case is report-hash only.) In a
50-engine regression this dimension fired on a real engine whose modify silently
dropped a sibling — and it is the **state** mismatch, not the report, that
distinguishes a genuine order-loss from a mere queue-order convention (a convention
leaves the sibling resting; a loss does not).

### Faithful engine-state forwarding — an adapter discipline

The book-state audit is only as honest as the adapter answering the queries.
`engine_query_best_bid` is an *adapter* symbol; a faithful adapter **forwards** it to
the engine's own accessor, or reads the engine's own book container, so the engine's
real state — bug and all — surfaces. An adapter that instead answers from an
**independent shadow** it maintains in parallel (or rebuilds from the report stream)
masks the engine: the shadow stays correct even when the engine's book is not,
degenerating the state audit into a re-check of the already-hashed report stream. The
discipline, applied across the reference adapters: answer `engine_query_*` from the
engine's own state; keep an adapter-side shadow only for what the engine genuinely
cannot report (e.g. a cancel/modify accept-reject signal some engines never expose),
never for the three queries. A worked proof: an adapter for a Python engine whose
`best_bid` was re-derived to skip cancelled orders **passed** the state audit on a
stale-cancel case; switching it to forward to the engine's own `_get_best_price` made
it **fail** — exposing the engine's real phantom-level bug — while a clean control
case still passed. All 134 adapters built across the survey (the 40 that ship in
`additional_references/` plus those held data-only) have been audited against this
discipline, and the shipped adapters follow it.

### What the gate still cannot reach

Even with the state audit, some real defects are beyond a report+state gate driven by
this ABI; they are recorded as *latent* in `CORRECTNESS_FINDINGS.md`, and the engines
that carry them are listed *conforming with fix* (the patch in the filed issue), not
clean. A defect is unreachable when its trigger is: **wall-clock time** (a FIFO
tiebreak that overflows only after ~25 days of resting); a **value outside the
encodable domain** (a cost that overflows only near a numeric type's ceiling, or a
phantom level that needs fractional quantities the integer ABI can't express); an
**operation the ABI doesn't carry** (fill-or-kill, market orders); or a
**memory-safety fault whose output is byte-correct** (a use-after-free that reads
still-valid bytes — only a sanitizer, not a hash or a query, sees it). Separately, a
defect that corrupts only a *report* while the book stays correct (an unknown-id
cancel acked instead of rejected) is caught by the report-hash dimension, not the
state audit — there is no wrong book state to observe.

## What it deliberately does NOT test

Anything that turns on a **convention** rather than a hard invariant. The clearest
example is what a `modify` does to queue priority: whether a same-price quantity
*decrease* keeps priority (production exchanges) or is treated as cancel+reinsert
(many engines), and likewise how an *increase* or a
same-price reprice re-orders the queue. These are valid choices engines differ
on — quantity is conserved, nothing is lost, only the queue order changes — so
the harness contract leaves them unspecified and the canonical workload doesn't
depend on them. Including such a case would wrongly fail a conforming engine, so
the gate omits them. (Modify *is* tested where it does not turn on priority — a
crossing modify must match, a partial residual must reprice, a stale modify must
reject.)

## Engine bug vs. our adapter

A divergence the gate observes can be the **engine's** or **ours**: every engine
is driven through a hand-written adapter (a shim that maps the harness ABI to the
engine's API and synthesises the report stream). A defect the harness sees may be
a real engine bug the adapter faithfully reflects, or an artifact the adapter
introduces. Two such artifacts were caught this way — an adapter that pruned a
*partially*-filled maker on every fill, and one whose liveness shadow never
evicted a *fully*-filled maker — both surfaced as "lost order" / "stale" reports
while the engines were correct. Before a divergence is attributed to an engine,
it is reproduced against the engine's **native API**, bypassing the adapter.

## Running it

```sh
python3 scripts/conformance_check.py --consensus            # print the oracle (the reference engines must agree)
python3 scripts/conformance_check.py /path/to/engine.so     # PASS/FAIL per case + verdict
```

The oracle is the live byte-identical consensus of independent public conformers —
fast ones (cpp_orderbook / llc993 / hroptatyr-clob) that reproduce the reference
hashes — recomputed and cached from the shipped adapters, so the gate needs no
checked-in golden file. Each case carries a report-hash consensus and, where the
conformers also agree on book state, a state-audit consensus; an engine must match
both. A case where the conformers disagree is reported `NO-CONSENSUS` and skipped —
that input is outside the contract and cannot be a gate.
