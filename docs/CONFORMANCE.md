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
**byte-identical agreement of three independent engines** (Liquibook,
QuantCup, Exchange-core). An engine conforms only if its report-stream hash
matches that consensus on **every** case. The battery (see the source for exact
sequences):

| Case | Invariant |
|:-----|:----------|
| `cancel_middle_then_sweep`, `cancel_head_then_sweep` | cancelling a non-tail order in a same-price FIFO must not lose the orders behind it |
| `deep_fifo_scattered_cancel` | FIFO integrity after several non-adjacent cancels in a deep queue |
| `multi_level_ioc_sweep`, `exact_multilevel_boundary` | a marketable order fills across price levels best-price-first, exact-fill leaves nothing resting |
| `sweep_residual_becomes_bbo` | a non-IOC aggressor's residual rests as the new touch and is then fillable |
| `modify_into_cross` | a modify that reprices through the spread must match, not rest crossed |
| `partial_fill_then_cancel`, `modify_partially_filled_residual` | a partial-fill residual is conserved and remains cancel/modify-able |
| `full_fill_then_stale_cancel`, `stale_modify_after_full_fill` | a cancel/modify of a fully-filled (non-resting) order must be **rejected**, not acked |
| `fifo_priority`, `two_level_cancel_sell_priority` | price priority across levels, FIFO time priority within a level |
| `reuse_id_after_cancel` | the id of a cancelled order is free and reusable |

These are all **hard invariants** — properties every correct price-time-priority
book must satisfy, regardless of implementation.

## What it deliberately does NOT test

Anything that turns on a **convention** rather than a hard invariant. The clearest
example is what a `modify` does to queue priority: whether a same-price quantity
*decrease* keeps priority (production exchanges) or is treated as cancel+reinsert
(many engines, including FlashOne), and likewise how an *increase* or a
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

The oracle is the live byte-identical consensus of three independent engines,
recomputed (and cached) from the shipped reference adapters, so the gate needs no
checked-in golden file. A case where those three engines themselves disagree is
reported `NO-CONSENSUS` and
skipped — that input is outside the contract and cannot be a gate.
