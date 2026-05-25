# Discoveries

While building reference adapters to demonstrate how third-party matching
engines are wrapped behind the harness's `matching_engine_api.h` contract, we
surveyed eight publicly available matching engines (six C++, one Rust, one Go)
and recorded the observations below. They are reported here so an integrator
considering one of these engines, or planning a similar in-house benchmark,
can pick up where we finished rather than re-deriving the same findings.

**This document is a snapshot, not a judgment.** Each observation describes
the upstream commit listed under *Snapshot* below; a project's current
`main` may already differ. The framing throughout is factual and scoped to
what the harness measures on this snapshot and what we read in that source.
We draw no conclusion about engineering quality or fitness for any specific
use case; the projects' designs reflect their authors' goals, which may
differ from ours.

The eight adapter sources live in
[`additional_references/`](additional_references/); each `build.sh` clones
its upstream at the pinned commit so any observation here is reproducible.

## Snapshot

Measurements taken on **2026-05-24** against these upstreams:

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

## Selection criterion

We surveyed open-source matching engines that satisfied at least one of:

- significant public traction (>100 GitHub stars), or
- a high-throughput claim (>10 M orders/sec) advertised in the project's
  README, or
- adoption as a teaching reference (large tutorial / educational reach).

The eight engines that met the criterion at integration time:

- robaho/cpp_orderbook — <https://github.com/robaho/cpp_orderbook>
- jxm35/LimitOrderBook-MatchingEngine — <https://github.com/jxm35/LimitOrderBook-MatchingEngine>
- PIYUSH-KUMAR1809/order-matching-engine — <https://github.com/PIYUSH-KUMAR1809/order-matching-engine>
- mansoor-mamnoon/limit-order-book — <https://github.com/mansoor-mamnoon/limit-order-book>
- chronoxor/CppTrader — <https://github.com/chronoxor/CppTrader>
- Tzadiko/Orderbook — <https://github.com/Tzadiko/Orderbook>
- joaquinbejar/OrderBook-rs — <https://github.com/joaquinbejar/OrderBook-rs>
- geseq/orderbook — <https://github.com/geseq/orderbook>

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

- The report-stream hash differs from the three-baseline consensus on **all
  five scenarios**. Sample diff (format `type,seq,price,qty,maker,taker`):
  ```
  baselines:  1,249,32771,41,317969,881378
  robaho:     1,249,32493,41,317969,881378
  ```
  Same fill (quantity, maker, taker); different reported price.
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

**Trade hook never invoked.** `MDFeed/include/publisher/MDAdapter.h:29`
declares `notify_trade(trade_id, price, quantity, buyerAggressed)` on the
market-data publisher interface; `TryMatch` in `OrderBook.cpp` does not call
it. Even if it were called, the signature carries no per-fill maker/taker
identities — only an aggressor-side flag — so the engine's market-data path
cannot satisfy the harness's `(maker, taker)` Trade-report contract. The
reference adapter applies a one-line source patch
(`__jxm35_adapter_trade_hook` inside `TryMatch`) at build time — without it
the harness contract cannot be honored at all.

**State-query divergence on every scenario.** A substantial fraction of the
192 state checks mismatch the Liquibook baseline's answers to
`GetBestBidPrice / GetBestAskPrice / GetBidQuantities / GetAskQuantities`.
The exact count varies by probe seed (re-randomised per run) and by
scenario; observed on this snapshot is roughly 65–190 of 192, lowest on
`flash-crash` and highest on `static`. The engine's queries are internally
consistent for jxm35 but do not agree with what an independent baseline
computes from the same input stream. We have not pin-pointed root cause.

**Report-stream divergence on dense-crossing scenarios.** The hash matches the
consensus on the three volatile scenarios (`swing-25`, `swing-40`,
`flash-crash`) but differs on `static` (10,126 trades vs the consensus 10,428
— 302 short, ~2.9%) and on `normal` (71,852 vs the consensus 71,851 — the
trade count matches within one, but the rest of the report stream diverges
enough to fail the hash). Why dense-crossing patterns trigger divergence and
volatile ones do not is something we did not trace to source.

### PIYUSH-KUMAR1809/order-matching-engine — asymmetric cached-best staleness

The buy-side matching loop in `MatchingStrategy.hpp` (around L74–92) can leave
the cached `bestAsk` pointing to a level the matcher just emptied. When an
aggressive buy exactly fills the current best-ask level and the incoming
order is exhausted at the same step, the inner break clears the level's mask
and order list, then the outer `if (incoming.quantity == 0) break;` skips the
`p++; bestAsk = p;` update. The post-loop corrective at L90
(`if (askMask.findFirstSet(bestAsk) >= MAX_PRICE) bestAsk = -1;`) only fires
when no higher asks remain — when higher asks DO remain it is a no-op.

The symmetric sell-side path (L141–161) uses a different corrective shape
(`!book.bidMask.test(book.bestBid)` walks down via `findFirstSetDown`) and
self-heals. The staleness is therefore asymmetric: buy aggressors can leave
stale `bestAsk` values; sell aggressors do not corrupt `bestBid`.

How it presents in the harness:

- The report-stream hash matches the consensus on all five scenarios — the
  trade events themselves are correct.
- The state audit catches the staleness probabilistically, depending on
  whether one of the 192 checks lands in a window after a buy-side fill
  that hit the path. The probe seed re-randomises each run. On `static`
  (small recycled level set) a handful of checks — roughly 1 to 10 of 192
  (median ~5) across observed runs — mismatch on every run. The other four scenarios
  mismatch 0 to a few of 192 and pass outright on most observed runs.

### mansoor-mamnoon/limit-order-book — no correctness findings

The engine produces a byte-identical report stream against the three-baseline
consensus on every scenario, and 192/192 state-audit checks match the
Liquibook baseline on every scenario. Of the eight projects surveyed, mansoor
and CppTrader are the two with a fully clean correctness signal.

### chronoxor/CppTrader — no correctness findings

CppTrader produces a byte-identical report stream against the three-baseline
consensus on every scenario, and 192/192 state-audit checks match the
Liquibook baseline on every scenario. Two operational details worth noting
for an integrator:

- `MarketManager::EnableMatching()` is **OFF by default**. Without it the
  engine silently rests every order without crossing — every aggressor would
  rest with full unfilled quantity and produce zero Trade reports. The
  reference adapter enables matching once after the order book is created.
- `MarketHandler::onExecuteOrder` fires **twice per fill**, first with the
  maker (resting) order, then with the taker (incoming). The reference
  adapter pairs consecutive callbacks into one harness Trade report and
  tallies the taker's filled quantity for IOC residual accounting.

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
continues consuming it first. The harness's canonical baseline holds
price-time priority as an invariant; OrderBook-rs as snapshotted at the
pinned commit does not.

How it presents in the harness:

- The report-stream hash **FAILs on `static`, `normal`, `swing-40`** and
  **PASSes on `swing-25` and `flash-crash`**. The split is what one would
  predict: scenarios with wider price excursions tend to consume each price
  level cleanly in one shot or barely touch it at all, so the partial-fill
  case is rarely exercised. Scenarios with heavier same-price queueing
  exercise it often enough to diverge.
- The state audit passes 192/192 on every scenario.
- Where the hash fails, the divergence is always a Trade-report
  `maker_order_id` swap at exactly one price level; per-trade total
  quantity, taker, price, and the order-ack / cancel-ack / modify-ack /
  reject streams all match canonical byte-for-byte. The hash mismatch is
  purely an identity question about which of several resting orders at the
  same price was the counterparty.

A fix in the engine would re-insert the partially-filled remainder at the
*head* (or carry a per-order ordinal that the match loop honors). The
adapter cannot work around this without reimplementing the matching loop
above the engine — at which point one is no longer measuring the engine.

### Tzadiko/Orderbook — matcher latency dominates the wall clock

Tzadiko/Orderbook is a clarity-first reference implementation often used
as a teaching example. The engine's matching path takes a member
`std::mutex` on every public mutator and walks a
`std::map<Price, std::list<OrderPointer>>` per side with
`std::shared_ptr<Order>` objects. Both choices are reasonable for an
educational framing but compound across the harness's ~2 M-event workload.

How it presents in the harness:

- **All five scenarios INFEASIBLE.** None of the five scenarios completes
  audit mode inside a 540 s per-scenario wall-clock budget. We could not
  determine correctness against the canonical report stream because no
  scenario finished.

The reference adapter applies two patches at build time to remove obvious
mismatches with the workload — the Windows-only `localtime_s` call in
`PruneGoodForDayOrders` (background thread, never wakes during the run) and
a `trades.reserve(orders_.size())` at the start of each match that allocates
~50k vector slots against a typical fill count of 0–10 — and answers
`engine_query_*` from a local shadow rather than the engine's
O(N_resting) `GetOrderInfos()`. These changes help materially but do not
close the gap to a workable wall-clock budget; the matcher itself is the
bottleneck.

The engine is well-shaped as an illustration of price-time-priority
bookkeeping (which is the role its educational framing implies); it is not
engineered for replay-grade throughput. We mention this because anyone integrating
Tzadiko/Orderbook into a realistic backtest or replay system will hit the
same wall and it may save them an afternoon.

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

## Throughput observations

The harness measures median throughput on dedicated cores (matcher and
drainer pinned via `--matcher-core` / `--drainer-core`) over ten 10⁶-NEW
workload runs per scenario. The workload is calibrated to U.S. equity microstructure (see
`docs/METHODOLOGY.md`): ~95% cancellation, 15% IOC, ~2% duplicate
cancels/modifies, GBM mid-price walk per scenario, and full inter-thread
report drainage on the timed path.

The figures the projects publish were measured under their own workloads,
with their own definitions of an operation. The table below records both
numbers so a reader can see the difference in workload — it is not a
comparison of like with like, and we have not attempted to reproduce the
projects' own benchmarks under their own conditions.

| Engine       | Harness `normal`, full report drainage | Published figure | Published workload (as described in the project) |
|--------------|----------------------------------------:|------------------:|--------------------------------------------------|
| robaho       | 3.70 M/s                                | 10–22 M/s         | insertion-focused micro-benchmark; ~50–70 ns per-op latency |
| piyush       | 2.27 M/s                                | ~160 M/s          | 10-symbol sharded; in-process trade callback on matcher thread |
| jxm35        | 1.86 M/s                                | 14 M/s            | per-op latency benchmark; trade events not emitted (see above) |
| mansoor      | ≤0.03 M/s                               | >20 M/s           | tight price band, no GBM mid-price walk, no cancels/modifies in the timed path |
| CppTrader    | 5.36 M/s                                | ~3.2 M/s          | NASDAQ ITCH replay, in-process callbacks |
| OrderBook-rs | 0.79 M/s (INVALID — hash FAIL)          | latency-focused   | tail-latency HDR-histogram bench suite; README also reports 200k ops/s mixed-workload and 19M ops/s hot-spot |
| Tzadiko      | infeasible (all 5)                      | not headlined     | tutorial repo; no published throughput target |
| geseq        | infeasible (>540 s / trial)             | 12.5–21 M/s, p50 170 ns | per-op micro-benchmark on a single Go goroutine |

A reader writing their own engine and measuring it the way the harness does
should expect numbers in the range above rather than the projects' published
figures. The factors that shift readings most are (a) whether reports are
drained to a separate thread on a separate core, (b) how broad the price band
the workload visits is, and (c) whether the workload contains cancels and
modifies at all.

Two notes on the newer adapter rows. **OrderBook-rs** completes the run in
~2.5 s wall time but its report-stream hash does not match the consensus on
`normal` — the throughput is real but the engine is INVALID on this scenario,
see *Correctness findings*. **Tzadiko** and **geseq** both exceed the
540 s per-trial wall-clock cap (the engines themselves are the bottleneck,
not the adapters); the figure is recorded as `infeasible` rather than a
throughput number.

### Reference baselines for calibration

The three open baselines (Liquibook, QuantCup, Exchange-core) that anchor
the byte-identical correctness consensus are not subjects of the survey
above — they were chosen as design-space anchors, not by their throughput
claims. Their numbers on the same workload appear here so a reader can see
where the harness lands on engines that have been studied widely.
FlashOne — Flash One Technologies' production engine, the harness publisher
— is included for reference; its `.so` is not publicly available, so the
numbers are reproducible only under a production license.

| Engine          | static | normal | swing-25 | swing-40 | flash-crash |
|-----------------|-------:|-------:|---------:|---------:|------------:|
| Liquibook       |  infeasible | 4.70 M/s |   4.75 M/s |   4.73 M/s |    4.75 M/s |
| QuantCup        |  6.99 M/s | 3.72 M/s |   0.70 M/s |   0.47 M/s |    0.35 M/s |
| Exchange-core   |  1.23 M/s | 1.69 M/s |   1.72 M/s |   1.72 M/s |    1.76 M/s |
| FlashOne        | 29.43 M/s | 30.15 M/s | 30.23 M/s | 29.96 M/s | 30.22 M/s |

Architectures: Liquibook is a price-keyed multimap per side; QuantCup is
a flat price-indexed array; Exchange-core is a direct-access order book on
the JVM (JNI per message). Each baseline exhibits a different sensitivity
to volatility — QuantCup's flat array is fastest on `static` and collapses
20× by `flash-crash`; Liquibook's price-keyed multimap is volatility-flat
at ~4.7 M/s on the four GBM scenarios but infeasible on `static`'s
dense-cross workload; Exchange-core is volatility-flat at ~1.7 M/s but
pays a JNI crossing per message. FlashOne is volatility-flat at ~30 M/s.
(Liquibook `static`: every trial ~0.03 M/s (~54–86 s wall) — the engine
itself is the bottleneck, not a software cap.) See
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
every message across that boundary. The largest effects:

- **Exchange-core** is much slower here: the harness crosses JNI on every message and re-launches the JVM fresh per scenario, while the paper runs Exchange-core fully in-
  process Java with three JIT warmup passes before each measured pass.
- **Liquibook and QuantCup** are *faster* here because the paper's apples-to-apples adapter carries reject-path bookkeeping the harness's simpler adapter does not need.
- **FlashOne** is also slightly down from the paper's Table 5 figures, by
  the same `.so` dispatch overhead applied symmetrically.

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

## Reproducing

Each observation in this document is reproducible from running code in this
repository:

```bash
bash additional_references/<name>_adapter/build.sh   # clones + builds
./harness --engine <name>_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

Where `<name>` is one of `piyush`, `mansoor`, `jxm35`, `robaho`, `cpptrader`,
`tzadiko`, `orderbookrs`, `geseq`. The build scripts pin each upstream to
the commits listed in *Snapshot*. The adapters themselves are not maintained
— if any upstream advances past the pinned commit the source-level
observations here may no longer apply; treat this document as a record of
one point in time.
