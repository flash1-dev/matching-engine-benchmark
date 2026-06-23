# jxm35_adapter — integration example

Wraps [jxm35/LimitOrderBook-MatchingEngine](https://github.com/jxm35/LimitOrderBook-MatchingEngine)
behind `api/matching_engine_api.h`.

Pinned commit: `b5984aacb1f9a1816855df4942752711866dbfbf`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`OrderBook<MDPublisher>` templated on a market-data publisher. Native APIs
visible from the adapter:

- `OrderBook::AddOrder(Order)` — inserts and matches internally.
- `OrderBook::AmendOrder(orderId, Order)` — cancel + re-add inside the
  engine.
- `OrderBook::RemoveOrder(orderId)` — synchronous, `void`.
- `OrderBook::ContainsOrder(orderId)` — `bool` lookup.
- `OrderBook::GetBestBidPrice()` / `GetBestAskPrice()` — `std::optional<long>`.
- `OrderBook::GetBidQuantities()` / `GetAskQuantities()` —
  `std::map<price, qty>` by-level totals.

Not provided natively: no IOC / FOK / POST-ONLY flags; no Ack/Reject report
types. The MD publisher API declares `notify_trade(...)` but `TryMatch` in the
matching loop never calls it, so the engine's own market-data feed publishes no
executions as shipped — a real defect, filed upstream and fixed by patch P1
below (see "Source patch"); the adapter additionally injects its own per-fill
hook (P0) to recover maker/taker identity.

## Adapter strategy

- **Trades** are surfaced by an injected per-fill hook (P0 in "Source
  patch"): `build.sh` adds one extern-C call inside the matching loop after
  a fill is recorded,

  ```cpp
  __jxm35_adapter_trade_hook(restingOrder.OrderId(), incomingOrder.OrderId(),
                             matchedQty, opposingPrice);
  ```

  which appends to a global vector (the single matcher thread is the only
  caller) that the adapter drains into Trade reports after `AddOrder`
  returns. This is adapter instrumentation only — the **two engine
  correctness fixes** the harness classifies this snapshot "with fix" for
  (P1, P2) are separate; see "Source patch" below for all three. Every patch
  is idempotent and `build.sh` reapplies it on rerun.

- Instantiates `OrderBook<mdfeed::NullMarketDataPublisher>` (one of the two
  publishers the engine already explicitly instantiates) and ignores the
  publisher's callbacks — trade information comes from the hook above.

- **IOC**: `AddOrder` (residual rests as Limit) → detect residual →
  `RemoveOrder` + emit `CancelAck`.

- **Modify**: the engine's native `AmendOrder` (its own remove + re-add at
  the new price/qty, queue priority lost — exactly the harness contract).
  The crossing trades inside the re-add half carry the modify's seq via the
  hook.

- **Rejects**: adjudicated by the engine's own existence API —
  `ContainsOrder(oid)` gates both cancel and modify.

- A per-order shadow (a flat vector indexed by the dense harness order id,
  `oid -> {price, side, remaining}`) echoes side/price/qty on
  CancelAck/ModifyAck — the payload the engine's APIs do not return.

## Source patch

`build.sh` applies **three** source patches to the cloned engine, each after
`git reset --hard` to the pin (so the reset can never clobber them) and each
idempotent with its own marker guard (fails loud if its anchor drifts). They
are **not** "a single one-line fix" — one is adapter instrumentation and **two
are the engine correctness fixes** that make this snapshot the *fixed* engine.
Both correctness defects are filed upstream and recorded in
[`CORRECTNESS_FINDINGS.md`](../../CORRECTNESS_FINDINGS.md) (verdict: *Fix
submitted upstream … fix verified, VALID ×5 across 100 seeds*).

- **P0 — per-fill maker/taker hook** (adapter instrumentation,
  `OrderBook.cpp`). `OrderBook<>::TryMatch` is the only fill site and never
  exposes *which two orders* traded, so the patch forward-declares
  `extern "C" void __jxm35_adapter_trade_hook(maker_id, taker_id, qty, price)`
  and inserts one call to it right after the matched-quantity bookkeeping. The
  adapter implements the hook and drains it into Trade reports (same "a hook
  the engine should call but doesn't" pattern as `kautenja_adapter`). This adds
  only the one emit point; matching logic, prices, and quantities are otherwise
  byte-identical to the pinned source.

- **P1 — CORRECTNESS: emit the executions the engine never published**
  (`OrderBook.h` + `OrderBook.cpp`). Filed upstream as
  [jxm35/LimitOrderBook-MatchingEngine#1](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1).
  `TryMatch` fills the book and bumps `matchedQuantity_` but never calls
  `notify_trade`, so the engine's own market-data feed sees price-level updates
  yet **no trades**. The class also has no trade-id counter. The patch (a) adds
  a `nextTradeId_` member next to `matchedQuantity_`, and (b) calls
  `md_adapter_.notify_trade(...)` at the fill site, beside the existing
  `notify_price_level_change` call, so the feed now publishes executions
  (price, qty, aggressor side). This restores the engine's own feed so the
  shipped benchmark engine is the fixed engine; the adapter itself reads fills
  via P0 and uses the `Null` publisher, but the defect is real for any consumer
  of the engine's MD path.

- **P2 — CORRECTNESS: double-unlink in `RemoveOrder`** (`OrderBook.cpp`, the
  decisive fix). Filed upstream as
  [jxm35/LimitOrderBook-MatchingEngine#2](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/2).
  Cancelling/modifying a **non-head** order on a level of ≥2 orders unlinked
  the node **twice**: a hand-splice touched only `prev`/`next` (skipping the
  owning `Limit`'s counters), and then `limit->RemoveOrder()` re-walked from
  `head_` to do the real `size_`/`orderQuantity_` accounting — but the
  hand-splice had already pulled the target out of that chain, so the walk
  bailed ("Order not found") and the level's counters were never decremented.
  The level then overstated depth and hid makers from `TryMatch` (51 fewer
  trades than the consensus on a deep book; `AmendOrder` is cancel + reinsert,
  so modify inherits it). The fix drops the hand-splice block so
  `limit->RemoveOrder()` is the **sole** unlink (the head path already relied
  on it). This is the cancel-path corruption the harness flags in the findings
  table.

## Build / run

Requires a C++23 compiler (GCC 13+ / Clang 17+, for `std::expected`) and the
`{fmt}` library (`libfmt-dev`); `build.sh` applies the three `python3` source
patches above (see "Source patch") before compiling.

```bash
bash additional_references/jxm35_adapter/build.sh
./harness --engine jxm35_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into
`third_party/jxm35_limit_order_book_matching_engine/` at the pinned commit
and applies the three patches above to the cloned `OrderBook.cpp` /
`OrderBook.h` in place. Use `ME_JXM35_SRC=/path/to/checkout` to skip the clone
(the script still applies the patches to your checkout — `git reset --hard`
first if you want to undo).
