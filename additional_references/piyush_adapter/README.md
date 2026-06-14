# piyush_adapter — integration example

Wraps [PIYUSH-KUMAR1809/order-matching-engine](https://github.com/PIYUSH-KUMAR1809/order-matching-engine)
behind `api/matching_engine_api.h`.

Pinned commit: `033d7859186bdc7e265b76883da5515722f7f249`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
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
no modify, no Ack/Reject events at all.

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

## Build / run

```bash
bash additional_references/piyush_adapter/build.sh
./harness --engine piyush_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/piyush_order_matching_engine/`
at the pinned commit. Use `ME_PIYUSH_SRC=/path/to/checkout` to skip the clone.
