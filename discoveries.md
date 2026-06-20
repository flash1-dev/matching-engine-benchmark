# Discoveries

While building reference adapters to demonstrate how third-party matching
engines are wrapped behind the harness's `matching_engine_api.h` contract, we
surveyed eleven publicly available matching engines (six C++, three Rust, two Go)
and recorded the observations below. They are reported here so an integrator
considering one of these engines, or planning a similar in-house benchmark,
can pick up where we finished rather than re-deriving the same findings.

This document is a snapshot, not a judgment. Each observation describes
the upstream commit listed under *Snapshot* below; a project's current
`main` may already differ. The framing throughout is factual and scoped to
what the harness measures on this snapshot and what we read in that source.
**We draw no conclusion about engineering quality or fitness for any specific
use case; the projects' designs reflect their authors' goals, which may
differ from ours.**

The eleven adapter sources live in
[`additional_references/`](additional_references/); each `build.sh` clones
its upstream at the pinned commit so any observation here is reproducible.

## Snapshot

Source observations were first recorded on **2026-05-24** (the original
eight) and **2026-06-09** (three high-throughput-claim engines, added below)
against these upstreams. On **2026-06-11** two things changed and every
throughput figure and verdict in this document was re-measured in a single
confined session on the result:

- **All adapter shims were reworked** to a minimal-overhead form —
  flat-vector id translation, no adapter-side locks or per-message
  allocation, and the engine's own API wherever the engine provides one
  (each adapter README documents its mapping). One verdict was corrected in
  the process (Tzadiko, below).
- **The canonical workload was re-anchored.** The generator implements the
  paper benchmark's construction; how deep a standing book a run carries is
  a property of the seed's price-path realisation (see
  `docs/METHODOLOGY.md`, *The standing book*). The previously published
  canonical seed produced realisations whose moving scenarios ran against a
  nearly empty resting book — flattering engines whose per-order costs grow
  with standing depth, and exercising none of the resting-set pressure the
  paper's own benchmark runs carried. The canonical seed is now **23**,
  chosen so each scenario's realisation matches the paper benchmark's
  standing-book and fill profile scenario for scenario; a marketable-order
  fraction that the earlier harness workload added on top of the paper's
  model was removed at the same time. All five reference hashes were
  regenerated from the three-baseline consensus. Figures measured before
  this revision — including earlier revisions of this document — are not
  comparable to the table below.

| Project                                         | Lang | Pinned commit                              | Upstream as of |
|-------------------------------------------------|------|--------------------------------------------|----------------|
| robaho/cpp_orderbook                            | C++  | `f42358145e40015f709f1caa04670f88c8b8be40` | 2025-07-31     |
| jxm35/LimitOrderBook-MatchingEngine             | C++  | `b5984aacb1f9a1816855df4942752711866dbfbf` | 2025-10-11     |
| PIYUSH-KUMAR1809/order-matching-engine          | C++  | `033d7859186bdc7e265b76883da5515722f7f249` | 2026-01-11     |
| mansoor-mamnoon/limit-order-book                | C++  | `78e1fb0e0563388456e5030d858ef43d6407bed3` | 2025-08-29     |
| chronoxor/CppTrader                             | C++  | `831d10e2a6dd96ac7b063f1d418f6563cbf74c50` | 2026-05-03     |
| Tzadiko/Orderbook                               | C++  | `dd136dd219ead95796f0e396e9e1395542bf673f` | 2024-04-06     |
| joaquinbejar/OrderBook-rs                       | Rust | `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` | 2026-05-03     |
| geseq/orderbook                                 | Go   | `3b9e9cd93cbaac02ba8359d2c3443a962d04c05f` | 2024-11-02     |
| philipgreat/lighting-match-engine-core          | Rust | `381aeda4298524758db37d90c9a69f0fa5c8ca6c` | 2026-04-21     |
| solarpx/limitbook                               | Rust | `943eadc181d1e35a26abaa5217eeb32bf3304267` | 2025-08-08     |
| ejyy/femto_go                                   | Go   | `46667a95064bd028e8f0ec1bc6a2f776d86721e3` | 2025-09-16     |

## Selection criterion

We surveyed open-source matching engines that satisfied at least one of:

- significant public traction (>100 GitHub stars), or
- a high-throughput claim (>10 M orders/sec) advertised in the project's
  README, or
- adoption as a teaching reference (large tutorial / educational reach).

The eleven engines that met the criterion at integration time (the last three
were added on 2026-06-09, each on the strength of a >10 M orders/sec claim):

- robaho/cpp_orderbook — <https://github.com/robaho/cpp_orderbook>
- jxm35/LimitOrderBook-MatchingEngine — <https://github.com/jxm35/LimitOrderBook-MatchingEngine>
- PIYUSH-KUMAR1809/order-matching-engine — <https://github.com/PIYUSH-KUMAR1809/order-matching-engine>
- mansoor-mamnoon/limit-order-book — <https://github.com/mansoor-mamnoon/limit-order-book>
- chronoxor/CppTrader — <https://github.com/chronoxor/CppTrader>
- Tzadiko/Orderbook — <https://github.com/Tzadiko/Orderbook>
- joaquinbejar/OrderBook-rs — <https://github.com/joaquinbejar/OrderBook-rs>
- geseq/orderbook — <https://github.com/geseq/orderbook>
- philipgreat/lighting-match-engine-core — <https://github.com/philipgreat/lighting-match-engine-core>
- solarpx/limitbook — <https://github.com/solarpx/limitbook>
- ejyy/femto_go — <https://github.com/ejyy/femto_go>

## How the harness probes correctness

The harness produces three signals per engine per scenario:

1. **Report-stream hash** — SHA-256 over the engine's full output stream
   (OrderAck, Trade, CancelAck, ModifyAck, CancelReject, ModifyReject)
   stable-sorted by `(sequence_number, type)`. The reference value is the
   byte-identical agreement of three independent baselines (Liquibook,
   QuantCup, Exchange-core) — three unrelated codebases producing the same
   stream on the same input.
2. **State audit** — 192 random-point `engine_query_*` checks during the run
   (64 probe indices × 3 queries each: `best_bid`, `best_ask`, `depth_at`),
   compared against a baseline replay's answers (Liquibook in the published
   runs). Catches book-state divergence that does not surface in the report
   stream.
3. **Throughput** — median of 10 trials on dedicated cores (matcher and
   drainer pinned via `--matcher-core` / `--drainer-core`), measured under
   the harness's calibrated workload (see `docs/METHODOLOGY.md`).

The correctness findings below all came from (1) and (2). The throughput
observations live in their own section and are framed as side-by-side
measurements under different workloads, not as a comparison of like with like.

### What VALID and INVALID mean here

The harness's verdict is mechanical. A run is **VALID** only when the engine's
full report stream reproduces the three-baseline consensus hash *and* the 192
random-point state-audit probes match a baseline replay (see
`docs/ANTI_CHEAT.md`). Any byte difference — a different trade price, a
different maker id, an extra or a missing fill — makes the run **INVALID**. So
INVALID means *"this engine's output diverges from the consensus of three
independent baselines,"* and nothing more; it is not a judgment of engineering
quality.

Those divergences are not all the same kind, and each finding below says which
kind it is:

- **Hard-invariant violation** — the output breaks a rule no order book can be
  configured out of: quantity is not conserved, an order fills past its resting
  size, or a trade prints through the book. limitbook over-matches ~4.3×; the
  un-patched form of geseq trades through the book. These are wrong against the
  engine's own contract, not only against ours.
- **Price-time-priority violation (quantity-conserving)** — quantity is conserved
  and the engine is internally coherent, but one field breaks the price-time
  priority that regulated equity limit-order books publish and the three
  baselines enforce. The two we saw fail differently: robaho stamps each cross
  with the wrong execution price (the lower of the two limits — i.e. the
  aggressor's own price on a sell-side cross — rather than the maker's), pairing
  the right two orders at the wrong price; OrderBook-rs matches a *later* arrival
  ahead of a partially-filled maker that should keep its queue position, so the
  counterparty itself is wrong. Both fail the consensus hash — INVALID — and are
  milder than the class above only in that no quantity is fabricated and no trade
  prints through the book, not because the divergence is a defensible convention.
- **Engine-state corruption** — the engine's internal bookkeeping breaks in
  a way that can end a run rather than (only) bend the output. We observed
  one such defect in CppTrader off the canonical path (its id index retained
  a fully-executed order node with a null price-level pointer; a later cancel
  of that id segfaulted inside the engine); it has since been **fixed
  upstream** — the verified mechanics, provenance, and resolution are in
  [`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md). The canonical workload does not trigger it, so
  CppTrader's table cells carry ordinary verdicts.

We flag the distinction so a reader can weigh each divergence on its merits
rather than reading one INVALID as equivalent to another.

## Correctness findings

### robaho/cpp_orderbook — trade-price field

The fill price is computed in `orderbook.cpp:24` as:

```cpp
F price = MIN(bid->_price, ask->_price);
```

When the cross condition holds (`bid->_price >= ask->_price`), `MIN` returns
`ask->_price` regardless of which side is the aggressor. For a buy aggressor
this coincides with the maker (resting ask) price, which is the convention
used by regulated equity exchanges in the US, EU and UK and by the three
baseline engines. For a sell aggressor crossing a higher resting bid the maker
is the bid, but the expression still returns the ask — i.e. the aggressive
sell's own limit.

How it presents in the harness:

- The report-stream hash differs from the three-baseline consensus on the
  **four moving scenarios**. First divergence on `normal` (observed during
  development; format `type,seq,price,qty,maker,taker` — a sell at 33,559
  crossing a resting bid at 33,857):
  ```
  baselines:  1,76,33857,87,232140,932154
  robaho:     1,76,33559,87,232140,932154
  ```
  Same fill (quantity, maker, taker — and 62,474 trades on `normal`, exactly
  matching the consensus count on every scenario); different reported price.
  Because the consensus hash compares the maker's price byte-for-byte
  (rule 1 in `LLM_INSTRUCTIONS.md`), this is `Verdict: INVALID` — a wrong
  execution price (a price-priority violation), not a quantity-conservation
  violation.
- On `static` the hash **passes**: that scenario's sparse fills are one-tick
  modify reprices meeting at the mid, where bid price equals ask price, so
  `MIN` of the two *is* the maker's price and the fill is correct.
- The state audit passes 192/192 checks on every scenario. Book bid/ask/depth
  bookkeeping is internally consistent — the divergence is purely in the
  trade-price field.

The repository's README and source contain no comment near `orderbook.cpp:24`
or elsewhere describing an alternative price-priority convention. We mention
this because the difference would matter downstream of any system that
consumes trade prices from the engine.

This observation is also pedagogically useful for harness users: a state
audit alone (without a multi-engine consensus hash) would not catch this
class of divergence.

The reference adapter applies one build-time patch unrelated to the
above: `exchange.h` and `bookmap.h` declare `std::vector<const std::string>`,
which is ill-formed under C++20; a `sed` in `build.sh` strips the `const`.
The affected accessors are not called by the adapter, so the patch is
semantically null.

### jxm35/LimitOrderBook-MatchingEngine — three independent observations

**Trade hook never invoked.** `lib/MDFeed/include/publisher/MDAdapter.h:29`
declares `notify_trade(trade_id, price, quantity, buyerAggressed)` on the
market-data publisher interface; `TryMatch` in
`lib/OrderBook/src/core/OrderBook.cpp` does not call it. Even if it were called, the signature carries no per-fill maker/taker
identities — only an aggressor-side flag — so the engine's market-data path
cannot satisfy the harness's `(maker, taker)` Trade-report contract. The
reference adapter applies a one-line source patch
(`__jxm35_adapter_trade_hook` inside `TryMatch`) at build time — without it
the harness contract cannot be honored at all.

**State-query divergence on every scenario.** A substantial fraction of the
192 state checks mismatch the Liquibook baseline's answers to
`GetBestBidPrice / GetBestAskPrice / GetBidQuantities / GetAskQuantities`.
The exact count varies by probe seed (re-randomised per run) and by
scenario; observed on this snapshot: roughly 88–185 of 192 across the five
scenarios (lowest on `flash-crash`, highest on `static`). The engine's queries are internally
consistent for jxm35 but do not agree with what an independent baseline
computes from the same input stream. We have not pin-pointed root cause.

**Missed crossings.** On every scenario the engine matches slightly less
than the consensus: 62,088 trades vs 62,474 on `normal`, 776 vs 827 on
`static`, within a handful on the swings. The first divergence on `normal`
(observed during development) is the shape of all of them: at seq 1,499 the
consensus fills 31 shares against resting order 447431 and then rejects that
order's stale modify and duplicate cancel (it is gone); jxm35 misses the
crossing, leaves 447431 resting, and instead acks the modify and the cancel. Each missed fill thus
also flips later reject reports into acks, so the stream diverges more
than the trade-count deficit alone suggests. During development we also ran
a deeper-standing-book stress variant of the workload, where the same
under-matching scaled dramatically (tens of percent of all crossings missed
and throughput collapsing with resting-set size); the canonical workload
exercises the defect only at the margin. We did not trace it to source.

Taken together these put jxm35 at `Verdict: INVALID` under the harness's
mechanical criterion — the state-audit probes mismatch the baseline on every
scenario, and the report-stream hash fails on every scenario. We did not
trace either to a specific mechanism, and the cause may lie in the engine,
in the required trade-hook patch, or in a query-semantics difference rather
than a book-state error. We therefore record jxm35 as an **untraced
divergence** — neither the hard-invariant nor the price-time-priority class above —
an observation rather than an attributed cause.

### PIYUSH-KUMAR1809/order-matching-engine — asymmetric cached-best staleness

The buy-side matching loop in `MatchingStrategy.hpp` (L43–89, with the
post-loop corrective at L90–92) can leave
the cached `bestAsk` pointing to a level the matcher just emptied. When an
aggressive buy exactly fills the current best-ask level and the incoming
order is exhausted at the same step, the inner break clears the level's mask
and order list, then the outer `if (incoming.quantity == 0) break;` skips the
`p++; if (p > book.bestAsk) book.bestAsk = (p < OrderBook::MAX_PRICE) ? p : -1;`
advance (L86–88). The post-loop corrective at L90–92
(`if (book.askMask.findFirstSet(book.bestAsk) >= OrderBook::MAX_PRICE) book.bestAsk = -1;`)
only fires when no higher asks remain — when higher asks DO remain it is a no-op.

The symmetric sell-side path (L141–161) uses a different corrective shape
(`!book.bidMask.test(book.bestBid)` walks down via `findFirstSetDown`) and
self-heals. The staleness is therefore asymmetric: buy aggressors can leave
stale `bestAsk` values; sell aggressors do not corrupt `bestBid`.

How it presents in the harness:

- The report-stream hash matches the consensus on all five scenarios — the
  trade events themselves are correct (62,474 of 62,474 on `normal`).
- The state audit catches the staleness on every moving scenario, with
  counts that vary by probe seed (re-randomised per run); observed on this
  snapshot: roughly 4–14 of 192 on each moving scenario.
  `static` passes 192/192 outright — with a fixed mid the cached best-ask
  can go stale only onto the level it already pointed at, so the stale value
  is still the right answer.

### mansoor-mamnoon/limit-order-book — no correctness findings

The engine produces a byte-identical report stream against the three-baseline
consensus, with 192/192 state-audit checks matching, on every scenario it can
complete inside the wall-clock budget (`swing-25`, the shuffled tape, exceeds
it). Of the eleven projects surveyed, mansoor is the only one with a fully
clean correctness signal and no source patch on every feasible scenario.

### joaquinbejar/OrderBook-rs — partial fills demote queue priority

In `pricelevel-0.7.0/src/price_level/level.rs::PriceLevel::match_order`,
when an incoming order partially fills a resting order the resting order is
`pop`'d from the level's FIFO queue, its visible quantity is decremented,
and the **partially-filled remainder is `push`'d back to the *tail* of the
queue** (`OrderQueue`, backed by a `SegQueue<Id>` for ordering plus a
`DashMap<Id, ...>` for storage):

```rust
// pricelevel-0.7.0/src/price_level/level.rs
if let Some(updated) = updated_order {
    // ...
    self.orders.push(Arc::new(updated));   // → tail of the FIFO
}
```

`OrderQueue::push` appends to the tail of `order_ids: SegQueue<Id>`, so the
partially-filled maker is now behind any later arrivals at the same price.
The next aggressor at that price will hit the later arrival first and may
fill it entirely before returning to the first order. Total resting quantity
at the price level is correct — which is why the state audit passes — but
the FIFO order of *which* resting order is consumed first is not preserved
across a partial fill.

Strict price-time priority — the rule virtually every regulated equity
exchange publishes for its standard limit-order book — requires that the
partially-filled maker stays at the head of the queue and the next aggressor
continues consuming it first. The harness's canonical baseline enforces
price-time priority — the published standard for equity limit-order books
(pro-rata venues allocate among same-price orders differently, but none
demotes a partially-filled maker behind later arrivals) — and OrderBook-rs as
snapshotted at the pinned commit does not. Where the
partial-fill case is exercised the report stream therefore diverges
(`Verdict: INVALID`): the counterparty identity is wrong (a time-priority
violation), with quantity, price and taker all correct.

How it presents in the harness:

- The report-stream hash **FAILs on all five scenarios**: a partially-filled
  maker with later arrivals queued behind it at the same price — the exact
  case the engine re-orders — occurs in every realisation, including
  `static`'s sparse modify-crossings. Total trade counts stay within one of
  the consensus (62,473 vs 62,474 on `normal`): the divergence is *which*
  resting order is the counterparty, occasionally splitting a fill
  differently, not how much is traded.
- The state audit passes 192/192 on every scenario.
- Where the hash fails, the divergence in the harness's runs reduces to a
  Trade-report `maker_order_id` swap at a single price level; per-trade total
  quantity, taker, price, and the order-ack / cancel-ack / modify-ack /
  reject streams all match canonical byte-for-byte. The hash mismatch is
  purely an identity question about which of several resting orders at the
  same price was the counterparty.

A fix in the engine would re-insert the partially-filled remainder at the
*head* (or carry a per-order ordinal that the match loop honors). The
adapter cannot work around this without reimplementing the matching loop
above the engine — at which point one is no longer measuring the engine.

### Tzadiko/Orderbook — the engine's own IOC type self-deadlocks on a partial fill

> **Correction (2026-06-11).** An earlier revision of this document reported
> Tzadiko/Orderbook as *infeasible on all five scenarios* and attributed that
> to matcher latency ("matcher latency dominates the wall clock"). That
> attribution was wrong. The runs were not slow — they were **hung in a
> deadlock inside the engine** that our adapter triggered by using the
> engine's own IOC order type. With the two-line correctness patch below, the
> engine completes every scenario and is **VALID on all five** at 3.39–3.7 M
> msgs/s. We keep the record of the correction here rather than silently replacing
> it.

Tzadiko/Orderbook is a clarity-first reference implementation often used as
a teaching example. Its `Orderbook::AddOrder` takes the book's non-recursive
member mutex (`std::scoped_lock ordersLock{ ordersMutex_ }`) and calls
`MatchOrders()` while holding it. At the tail of `MatchOrders()`, if the
front order at the best bid or best ask is `OrderType::FillAndKill` — i.e. a
partially-filled IOC whose residual is now resting — the engine cancels that
residual by calling the **public** `CancelOrder(...)`, which acquires
`ordersMutex_` again. `std::mutex` is non-recursive, so the first IOC order
that partially fills deadlocks the book on itself. The engine already has
the correct primitive for this context: `CancelOrderInternal`, the
already-locked variant its own bulk `CancelOrders` path uses under a single
lock acquisition. The two `MatchOrders` tail sites are the only
locked-context callers of the locking wrapper.

How it presents in the harness:

- **Un-patched:** every moving scenario hangs on the first partially-filled
  FillAndKill order and never completes (verified on `normal`; the workload
  is 15% IOC, and against a standing book some of those cross with partial
  liquidity early). `static` happens to survive — at a fixed mid a passive
  IOC never partially fills, so the lethal tail is never taken. An earlier
  revision of this document, measured on the old workload (whose marketable
  flow made partially-filled IOCs immediate on every scenario), recorded
  `infeasible (all 5)`.
- **With the two-site patch** (`CancelOrder` → `CancelOrderInternal` in the
  `MatchOrders` tail): all five scenarios **PASS** — the report stream
  reproduces the consensus hash byte-for-byte and the state audit returns
  192/192 on every scenario. Trades and end-of-call book state are exactly
  what an un-deadlocked public cancel would produce; the patch changes which
  lock wrapper is called, not what is cancelled.

The reference adapter applies the fix as its third build-time patch (same
pattern as jxm35's `notify_trade` and geseq's `compare()` patches), alongside
the two inherited ones: the Windows-only `localtime_s` call in
`PruneGoodForDayOrders` (background thread; the workload contains no
GoodForDay orders) and a `trades.reserve(orders_.size())` at the start of
each match that allocates ~50k vector slots against a typical fill count of
0–10. Harness IOC maps to the engine's own `FillAndKill`; modify uses the
engine's native `ModifyOrder`; audit queries are answered from the engine's
native `GetOrderInfos()`.

Two things are worth taking away. First, the engine's mutex-per-call +
`std::map<Price, std::list<OrderPointer>>` + `std::shared_ptr<Order>` design
— costs we previously blamed for the wall-clock failure — in fact sustains
3.39–3.7 M msgs/s on this workload, mid-pack among the engines surveyed here
and essentially flat from the deep `static` book to the flash-crash walk.
Second, anyone driving Tzadiko/Orderbook with its own `FillAndKill` type
will hit the deadlock as shipped: the snapshot commit's IOC path cannot
execute its partial-fill case at all. A downstream user who never sends
FillAndKill (or whose IOCs always fill completely or not at all) would never
see it.

### geseq/orderbook — multi-level crossings ignore the price predicate

In `pricelevel.go::processLimitOrder`, the inner matching loop iterates
across queues but does not re-check the price-cross predicate inside the
loop:

```go
// shipped form — predicate checked only once before entering
for orderQueue := pl.GetQueue();
    qtyLeft.GreaterThan(decimal.Zero) && orderQueue != nil;
    orderQueue = pl.GetQueue() {
    _, q := orderQueue.process(ob, takerOrderID, qtyLeft)
    qtyLeft = qtyLeft.Sub(q)
    qtyProcessed = qtyProcessed.Add(q)
}
```

Once the best queue crosses, the loop continues consuming subsequent queues
regardless of whether their price still crosses the aggressor's limit.
Concretely: a sell IOC at $32.86 that fills the maker at $32.96 (correctly)
keeps consuming the next-best maker at $32.74 (incorrectly).

How it presents in the harness:

- Without a source patch, the report-stream hash FAILs on all five
  scenarios and the state audit mismatches on most.
- With a one-line patch that adds `&& compare(orderQueue.Price())` to the
  loop header, all five scenarios PASS and state audit returns 192/192 on
  every scenario.

The reference adapter applies the patch as a build step (same pattern as
jxm35's `notify_trade` patch). We did not investigate whether the original
loop was an oversight or a deliberate simplification — the upstream README
does not mention multi-level crossing semantics either way. A downstream
consumer of fills running the un-patched engine would see fills at prices
that should not have crossed.

### philipgreat/lighting-match-engine-core — three issues surfaced by the cancel/modify path

The engine ships two order books behind a trait: a `DenseOrderBook` (a flat
`Vec` of price buckets, the path the README's "8 ns per order" figure
exercises) and a `SparseOrderBook` (`BTreeMap<price, bucket>` per side). The
dense book requires every price to fall inside a fixed on-tick window or the
order is rejected; the harness's GBM workload spans the full tick range, so the
adapter drives the sparse book — the only one of the two that can represent the
flow at all.

Run against the sparse book, the harness's cancel / modify / IOC lifecycle
surfaced three issues the engine's add-and-match micro-benchmark does not
exercise:

- A cancelled order left at the front of a price-level queue is not pruned
  before the next match, so the matcher emits zero-quantity Trades against
  it. Un-patched on the canonical `normal`, the engine prints 73,363 Trade
  reports against the consensus's 62,474 — 10,889 extra, all zero-quantity
  phantoms against cancelled front orders (observed during development; first
  at sequence 1,499: `1,1499,34205,0,503555,…`, a 0-share print against
  cancelled order 503555).
- Because the harness models modify as cancel + reinsert, a modify that keeps
  the same price can leave a same-id tombstone in the bucket the live order is
  reinserted into; `cancel_order` located that dead duplicate via a plain
  `find(|o| o.order_id == id)` rather than the live reinserted order, so a
  later cancel could act on the wrong instance.
- The same cancel-and-reinsert idiom could leave the reinserted order
  *disowned by the engine's id index*: both books prune dead orders lazily at
  the front of each price bucket, and `prune_bucket_front` also erased each
  popped id from `order_map`. Every order popped there had already been
  removed from `order_map` at the moment it went inactive (`cancel_order` and
  the match loops disown immediately), so the erase was a no-op — except when
  a live order had since been reinserted under the same id, where it deleted
  the *live* order's index entry. The order kept resting and matching, but the
  engine could no longer answer for its id: cancels of it spuriously failed.

How it presents in the harness:

- Un-patched, the Trade stream on `normal` carries 10,889 zero-quantity
  phantom prints (73,363 reports vs the consensus's 62,474, from sequence
  1,499 on) with the tombstone-cancel divergences riding inside it; the third
  issue is latent in the same cancel-and-reinsert lifecycle and surfaces as
  soon as anything — an engine path or an integrator — relies on `order_map`
  to answer for a reinserted id, as this adapter's reject gating does.
- With three build-time patches — prune the cancelled front order before
  matching (the dense book already does this every iteration), add an
  `&& o.is_active()` guard to `cancel_order` so it targets the live instance,
  and drop the `order_map.remove` from `prune_bucket_front` so the id index
  holds exactly the resting set —
  all five scenarios PASS and the (untimed) state audit returns 192/192 on
  every scenario (the 60 s budget that `static` exceeds in the
  throughput row below does not apply to the audit).

The reference adapter applies all three patches at build time (same pattern as
jxm35's `notify_trade` and geseq's `compare()` patches) and documents them in
its README. Each is a minimal correctness fix on the cancel/match path and is
not expected to change the engine's throughput characteristics (the patches
were not separately benchmarked). We did not investigate whether any was a
deliberate simplification tied to the two-price benchmark configuration.

### solarpx/limitbook — partial fills do not decrement the resting maker

In `src/order_book.rs`, the inner matching loop computes
`fill_quantity = remaining_quantity.min(resting_order.quantity)` and decrements the
aggressor's remaining quantity and the level's cached volume, but never writes
`resting_order.quantity -= fill_quantity`. A resting order is removed only when
a fill consumes its *original* quantity, so a partially-filled maker keeps its
full size and can be filled again indefinitely.

This reproduces in the engine with no adapter involvement: rest a buy for 100,
hit it with a sell for 30 (fills 30), again for 30 (fills 30), then a sell for
100 fills the full 100 — where a correct book has 40 remaining.

How it presents in the harness:

- The report-stream hash FAILs on all five scenarios and the state audit
  mismatches on most (roughly 143–177 of 192 on the moving scenarios). On `normal`
  the engine emits 268,154 Trades against the 62,474-Trade consensus (~4.3×
  over-match), with correspondingly inflated cancel and modify rejects
  (orders are filled away before their cancel arrives). The first divergence
  (observed during development) is at sequence 147, and it is the defect in
  miniature: resting sell 889598 (53 shares) was partially filled for 14 at
  seq 125, leaving 39;
  the consensus fills those 39 at seq 147 — limitbook, which never
  decremented the maker, prints 53.

The reference adapter does not patch this. Unlike the engine fixes above, closing
it would mean rewriting the engine's matching loop, at which point one is no
longer measuring the engine; the throughput row below is recorded with the
engine's output as shipped and marked INVALID — a quantity-conservation
violation (the hard-invariant class above), not the milder quantity-conserving class.

## Throughput observations

The harness measures median throughput on dedicated cores (matcher and
drainer pinned via `--matcher-core` / `--drainer-core`) over ten 10⁶-NEW
workload runs per scenario. The workload is calibrated to U.S. equity
microstructure (see `docs/METHODOLOGY.md`): ~95% cancellation, 15% IOC, ~2%
duplicate cancels/modifies, a GBM mid-price walk with a standing book the
engine carries throughout, and full inter-thread report drainage on the
timed path. A couple of cells run just past 60 s of wall clock per trial (~60–62 s);
rather than the ten-trial median used elsewhere, each such cell was run once to
completion to record an actual figure. The harness column below reports each
engine's **worst-case** — its lowest rate across the five scenarios, with the
scenario that produced it — the same basis as the reference-baseline table (a
venue must survive its worst regime); the rows are ordered by each project's
published claim.

The figures the projects publish were measured under their own workloads,
with their own definitions of an operation. The table below records both
numbers so a reader can see the difference in workload — it is not a
comparison of like with like, and we have not attempted to reproduce the
projects' own benchmarks under their own conditions.

| Engine       | Harness worst-case (weakest scenario) | Published figure | Published workload (as described in the project) |
|:-------------|:--------------------------------------|:-----------------|:-------------------------------------------------|
| piyush       | 3.17 M/s, `flash-crash` (INVALID — state audit) | ~160 M/s | 10-symbol sharded; in-process trade callback on matcher thread |
| philipgreat  | 0.03 M/s, `static` (VALID with fix — 3 engine correctness patches) | ~125 M/s ("8 ns/order") | two-price pre-seeded dense array; in-process, add-and-match only |
| limitbook    | 1.15 M/s, `static` (INVALID — over-match) | 3–5 M/s limit, ~30 M/s market/cancel | Criterion single-op micro-benchmarks; in-process, fills returned by value |
| robaho       | 1.89 M/s, `swing-25` (INVALID — price field) | 10–22 M/s | insertion-focused micro-benchmark; ~50–70 ns per-op latency |
| geseq        | 1.57 M/s, `static` (VALID with fix — engine price-predicate patch) | 12.5–21 M/s, p50 170 ns | per-op micro-benchmark on a single Go goroutine |
| mansoor      | 0.03 M/s, `normal` (VALID ×5) | >20 M/s           | tight price band, no GBM mid-price walk, no cancels/modifies in the timed path |
| jxm35        | 2.20 M/s, `normal` (INVALID — untraced) | 14 M/s | per-op latency benchmark; trade events not emitted (see above) |
| femto_go     | 2.24 M/s, `normal` (VALID on `static`/`normal`; INVALID on others) | >10 M/s, ~70 ns | in-process Go bench; output drained on a sibling goroutine, same process |
| CppTrader    | 7.26 M/s, `normal` (VALID ×5) | ~3.2 M/s          | NASDAQ ITCH replay, in-process callbacks |
| OrderBook-rs | 0.13 M/s, `static` (INVALID — priority only) | latency-focused | tail-latency HDR-histogram bench suite; README also reports 200k ops/s mixed-workload and 19M ops/s hot-spot |
| Tzadiko      | 3.39 M/s, `flash-crash` (VALID with fix — engine deadlock patch) | not headlined     | tutorial repo; no published throughput target |

A reader writing their own engine and measuring it the way the harness does
should expect numbers in the range above rather than the projects' published
figures. The factors that shift readings most are (a) whether reports are
drained to a separate thread on a separate core, (b) whether the workload
makes the engine carry a standing book, (c) how broad a price range the
workload visits, and (d) whether the workload contains cancels and modifies
at all.

Notes on the rows above. **piyush**'s report stream is byte-identical on all
five scenarios; the INVALID is its cached-best staleness failing the state
audit on every moving scenario (see *Correctness findings*).
**OrderBook-rs** completes the run in ~4 s wall time but its report stream
diverges from consensus on every scenario (a price-time-priority violation —
wrong counterparty identity — quantities correct). **geseq** and **femto_go** are Go matchers reached through cgo: driven one
message at a time they run at ~0.007 M/s (≈100–330 s/trial), bounded by the
per-call cgo crossing into the Go runtime, not by their matching (see *Batch
delivery and the ABI-crossing tax* below). Measured the way the harness
recommends for a cgo engine — with `engine_on_batch` (`docs/METHODOLOGY.md`) —
geseq reaches 1.57–1.78 M/s and femto_go 2.24–2.50 M/s on their VALID scenarios,
their actual matcher throughput, and the table above reports those batched
figures. **geseq** reproduces the consensus byte-for-byte on all five scenarios;
**femto_go** matches on `static` and `normal` but diverges deterministically on
the three moving scenarios (its price-window limit — a re-run reproduced the
same computed hash, and the batched and one-at-a-time streams are byte-identical,
so the divergence is the engine's, not the delivery). **Tzadiko**,
recorded as not completing in an earlier revision of this table, is
corrected above: the runs were deadlocked inside the engine, not slow, and
with the two-site engine patch it is VALID on all five scenarios at
3.39–3.7 M/s, essentially flat from the deep `static` book to `flash-crash`
(see *Correctness findings*). **CppTrader** is VALID on all five scenarios
at 7.26–7.6 M/s — the fastest clean reference (a `ModifyOrder` defect it
surfaced off the canonical path has since been fixed upstream — see
[`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)). **mansoor** completes
`static` (1.97 M/s) and lands right at the 60 s budget edge on `normal`
(~0.03 M/s, ~60–61 s per run) with a byte-identical stream on both; the
wider scenarios are very slow.

On **philipgreat**: VALID on all five scenarios after the three
patches described above; its `normal` throughput of 4.43 M/s sits against a
published "8 ns per order" (≈125 M/s) figure measured on a two-price
pre-seeded dense array, and `static` falls to 0.03 M/s (~62 s/trial, past the 60 s
budget — a fixed-price stream concentrates the ~21,000-order standing book on a
handful of sparse-map levels whose buckets it scans linearly). **limitbook** is
INVALID on every scenario (the partial-fill over-match above — a
quantity-conservation violation) and measures 1.15–2.72 M/s (slowest on `static`) against a published
3–5 M/s limit-order / ~30 M/s market-and-cancel micro-benchmark.

### Reference baselines for calibration

The three open baselines (Liquibook, QuantCup, Exchange-core) that anchor
the byte-identical correctness consensus are not subjects of the survey
above — they were chosen as design-space anchors, not by their throughput
claims. Their numbers on the same workload appear here so a reader can see
where the harness lands on engines that have been studied widely.
FlashOne — Flash One Technologies' production engine, the harness publisher
— is included for reference; its `.so` is not publicly available, so the
numbers are reproducible only under a production license.

The rows are ordered by **worst-case throughput** — the lowest of each engine's
five scenario results — because a venue must survive its worst regime, not its
best; the scenario that produces each worst case is shown alongside:

| Engine        | Worst-case throughput | Weakest scenario |
|---------------|----------------------:|:-----------------|
| FlashOne      | 33.20 M/s             | `normal`         |
| Exchange-core | 1.40 M/s              | `flash-crash`    |
| QuantCup      | 0.57 M/s              | `flash-crash`    |
| Liquibook     | 0.03 M/s              | `static`         |

Architectures: Liquibook is a price-keyed multimap per side; QuantCup is
a flat price-indexed array; Exchange-core is a direct-access order book on
the JVM (JNI per message). Each baseline meets its own nemesis among the
scenarios — that is the point of having five. QuantCup is fastest while the
walk stays narrow (8.5 M/s on `static`) and collapses ~15× as it spreads
(0.57 M/s on `flash-crash`'s ~38,000-tick range). Liquibook is the mirror
image: mildly volatility-sensitive at 4.3–4.9 M/s on the moving scenarios,
and very slow under `static`'s ~21,000-order standing book (≈0.03 M/s,
~67 s per trial — past the 60 s/trial floor), where its node-per-resting-order
design pays most: every new order allocates a `SimpleOrder` on the timed path,
which the deepest book pays ~1M times. The slowness is the engine's design,
not a software cap. Exchange-core is comparatively flat at
~1.4–2.0 M/s, its per-message JNI cost amortized by the harness's batch
delivery (see *Batch delivery: measuring the matcher, not the boundary*,
below). FlashOne spans 33.2–44.5 M/s. Ranked by that nemesis — each engine's worst case — flatness
wins: Exchange-core's 1.40 M/s floor outranks QuantCup's 0.57 and Liquibook's
0.03 M/s even though it never tops a single scenario, and FlashOne's 33.20 leads
outright — an engine is only as fast as the regime it handles worst. See
`docs/METHODOLOGY.md` for the five scenarios and `scripts/run_challenge.py`
for the full sweep.

All builds in this tree use `-march=native` (the reference recipe in
`docs/INTEGRATION.md`), so each engine sees the host's instruction set. The
matchers are memory-latency-bound, so the host-specific tuning shifts
numbers by at most a few percent vs a portable `-O3` build (largest
observed on Graviton4: QuantCup `normal` +7.3%, all others within ±5%) —
the convention is for production-style realism rather than headroom.

### Why these numbers differ from the paper's Table 5

The paper's Table 5 measures each engine's matching algorithm in a
single-process apples-to-apples harness that bypasses the dynamic loader
and (for Exchange-core) the JNI boundary. The figures in this document are
from the public harness, which loads each engine as a `.so` and dispatches
every message across that boundary. The workload model is the same
construction in both, and as of the 2026-06-11 seed re-anchoring the
canonical realisations carry the same standing-book and fill regime
scenario for scenario (see `docs/METHODOLOGY.md`, *The standing book*) — so
the remaining differences are architectural:

- **Every engine** pays the `.so` indirect-dispatch boundary per message
  here, FlashOne included, where the paper compiles each engine into the
  measuring binary.
- The two generators draw from different portable-RNG streams, so the
  realisations are not message-for-message identical — comparable in
  regime, not byte-for-byte.

One harness-wide caveat applies to every row: in both perf and audit mode,
the harness probes the engine's book at 64 random points per run for
anti-cheat indistinguishability (so an engine cannot tell a measured run
from an audited one). The probe query *time* is subtracted from the timed
window via an `excluded_ns` accumulator, but the queries' incidental cache
and state effects on the matcher's working set between probes are not
subtractable. The residual is small and symmetric — it sits on every
engine equally — but it is real measurement overhead riding on top of
the engine's normal operation, and the paper's Table 5 numbers carry no
equivalent.

### Batch delivery: measuring the matcher, not the boundary

The per-message boundary costs just described — JNI on every Exchange-core call,
the `.so` indirect dispatch on every call for all engines — are a property of how
the harness *delivers* messages, not of the matchers. Driven one at a time, each
delivery pays its boundary cost, and for some runtimes that cost dominates.
Isolating each crossing with a null adapter — one that does no matching, only
crosses the boundary and acks — gives the per-message tax directly:

The optional `engine_on_batch` entry point (`docs/METHODOLOGY.md`) removes them by
delivering a run of messages per call — the engine loops internally and pays the
crossing once per run, not once per message — while the harness ends each run at
the random audit-probe indices so the state audit and the report-stream hash stay
byte-identical (every batched figure here is VALID on the same basis as a
one-at-a-time run). Measured that way (clean solo, ten-trial median):

- **geseq** 0.007 → **1.57–1.78 M/s** (~220×) and **femto_go** 0.007 →
  **2.24–2.50 M/s** (~325×) on their VALID scenarios — their real matchers, with
  the cgo tax amortized away.
- **Exchange-core**, with a Java-side batch method that crosses JNI once per run,
  gains **+10–38%** per scenario (worst case 1.20 → 1.40 M/s).
- A fast **native** engine gains only the ~2 ns dispatch — ~8% at the top of the
  range, nothing lower down; FlashOne's figure moves from 31.09 to 33.20 M/s for
  this reason, the C/C++/Rust baselines not measurably at all.

So the comparison tables report the batched figure for exactly the four engines it
moves materially (FlashOne, Exchange-core, geseq, femto_go); every other row is
delivery-invariant within measurement noise. (The *outbound* report path also
crosses the boundary — Go→C per report — and batching it too lifts the Go engines
a further ~30%, but the inbound crossing is the dominant tax and the figures here
amortize only it.) The net is that batch delivery measures each engine on its
matching algorithm rather than on the language boundary its matcher happens to sit
behind — the comparison the harness is for.

## Reproducing

Each observation in this document is reproducible from running code in this
repository:

```bash
bash additional_references/<name>_adapter/build.sh   # clones + builds
./harness --engine <name>_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

Where `<name>` is one of `piyush`, `mansoor`, `jxm35`, `robaho`, `cpptrader`,
`tzadiko`, `orderbookrs`, `limitbook`, `philipgreat`, `geseq`, `femtogo`.
The build scripts pin each upstream to
the commits listed in *Snapshot*. The adapters themselves are not maintained
— if any upstream advances past the pinned commit the source-level
observations here may no longer apply; treat this document as a record of
one point in time.
