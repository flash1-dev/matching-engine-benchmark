# piyush_adapter — integration example

Wraps [PIYUSH-KUMAR1809/order-matching-engine](https://github.com/PIYUSH-KUMAR1809/order-matching-engine)
behind `api/matching_engine_api.h`.

Pinned commit: `033d7859186bdc7e265b76883da5515722f7f249`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`Exchange` + `OrderBook` + `StandardMatchingStrategy`. Native APIs visible
from the adapter:

- `Exchange::submitOrder(Order)` — async submit through a spinlock-guarded ring to a
  per-shard worker thread.
- `Exchange::cancelOrder(symbolId, orderId)` — async, `void`, no
  success/failure return.
- `OrderBook::addOrder/cancelOrder` — the synchronous primitives behind the
  worker (the adapter calls `cancelOrder`; inserts go through the matching
  strategy, below).
- `TradeCallback` — fires from the worker thread with batched trades.

Not provided natively: no IOC / FOK / POST-ONLY (only `Limit` / `Market`),
no modify, no Ack/Reject events at all. The cached best-ask (`OrderBook::bestAsk`)
the BBO queries read goes stale after a buy clears the best-ask level — a real
engine defect, fixed by `build.sh` (see the source patch).

## Adapter strategy

- Drives `StandardMatchingStrategy::match()` + `OrderBook` **inline** rather
  than going through `Exchange`'s worker thread. Both code paths run the same
  matching algorithm; inline removes a thread-sync race that would otherwise
  confound the audit (the worker's `tradeBuffer` flush is not synchronous
  with `engine_flush()`).
- Synthesises **OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject**
  above the engine. A per-order shadow `oid -> {price, side, remaining,
  alive}` — a flat vector indexed by the dense 1-based harness order id —
  is updated from the matching strategy's returned fill list. The shadow is
  necessary: the engine's `cancelOrder` is `void` (silent on unknown ids)
  and exposes no existence or result API the adapter could ask instead.
- **IOC**: submit as Limit; if any residual remains, `cancelOrder` it and
  emit a `CancelAck` for the residual quantity.
- **Modify**: cancel + reinsert at the new price/qty.

## Source patch

`build.sh` applies **one** source patch to the engine, after `git reset --hard`
to the pin so the reset can never clobber it (idempotent — a marker guard makes
a re-apply a no-op; fails loud if its anchor drifts, so the fix can never
silently no-op):

- **Cached best-ask re-seat** (correctness, `src/MatchingStrategy.hpp`). This
  is the "with fix" correctness patch — the same defect filed upstream as
  [PIYUSH-KUMAR1809/order-matching-engine#9](https://github.com/PIYUSH-KUMAR1809/order-matching-engine/issues/9).
  In `StandardMatchingStrategy::match()`'s `OrderSide::Buy` branch, an
  aggressive buy that exhausts itself on the *same* step that empties the
  current best-ask level hits `if (incoming.quantity == 0) break;` and exits
  the outer `while` before the `p++` advance, leaving `book.bestAsk` pointing
  at the now-emptied, mask-cleared level. The original post-loop corrective
  only zeroes `bestAsk` when the *whole* ask side is gone (`findFirstSet`
  scans upward *inclusive*, so any higher still-set ask returns an index
  `< MAX_PRICE` and the guard stays false), so the stale ask survives. The
  patch replaces that two-line guard with `nextAsk =
  askMask.findFirstSet(bestAsk); bestAsk = (nextAsk >= MAX_PRICE) ? -1 :
  (Price)nextAsk;`, re-seating `bestAsk` to the lowest still-set ask
  at-or-above its current value (or `-1` when no asks remain) — mirroring
  `OrderBook::cancelOrder` and the sell side's existing self-heal (the bid
  branch already re-tests `bestBid` and walks down via `findFirstSetDown`,
  which is the asymmetry the finding calls out). It is a two-line edit that
  changes **only** the cached BBO state; it touches no fill, price, or quantity
  in the match loop.

The bug is state-only, not a trade defect: the next aggressor re-derives best
from the mask, so the **trade/report stream is byte-identical with or without
the patch on all five scenarios**. What the stale `bestAsk` corrupts is any BBO/
spread query between operations, which is exactly what the harness state audit
reads — so unpatched, the asymmetric staleness fails the state audit on the
moving scenarios. With the patch the engine is `VALID ×5` across 100 seeds (see
`CORRECTNESS_FINDINGS.md`).

## Build / run

```bash
bash additional_references/piyush_adapter/build.sh
./harness --engine piyush_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/piyush_order_matching_engine/`
at the pinned commit. Use `ME_PIYUSH_SRC=/path/to/checkout` to skip the clone.
