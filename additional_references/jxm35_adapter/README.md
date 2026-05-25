# jxm35_adapter — integration example

Wraps [jxm35/LimitOrderBook-MatchingEngine](https://github.com/jxm35/LimitOrderBook-MatchingEngine)
behind `api/matching_engine_api.h`.

Pinned commit: `b5984aacb1f9a1816855df4942752711866dbfbf`.

This adapter is one of eight worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
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
types. The MD publisher API declares `notify_trade(maker, taker, qty, price)`
but `TryMatch` in the matching loop does not call it, so per-fill maker/taker
identity cannot be reconstructed through the engine's MD path as shipped.

## Adapter strategy

- `build.sh` patches a single line into the cloned `OrderBook.cpp`,
  injecting one extern-C hook call inside the matching loop after a fill is
  recorded:

  ```cpp
  __jxm35_adapter_trade_hook(restingOrder.OrderId(), incomingOrder.OrderId(),
                             matchedQty, opposingPrice);
  ```

  The hook appends to a thread-local vector that the adapter drains into
  Trade reports after `AddOrder` returns. Nothing else in the engine source
  changes; the patch is idempotent and `build.sh` reapplies it on rerun.

- Instantiates `OrderBook<mdfeed::NullMarketDataPublisher>` (one of the two
  publishers the engine already explicitly instantiates) and ignores the
  publisher's callbacks — trade information comes from the hook above.

- **IOC**: `AddOrder` (residual rests as Limit) → detect residual →
  `RemoveOrder` + emit `CancelAck`.

- **Modify**: explicit cancel (`RemoveOrder`) + emit `ModifyAck` + re-submit at
  the new price/qty. The crossing trades then carry the modify's seq via the
  hook.

- Shadow map for the reject path and CancelAck/ModifyAck side/price echo.

## Build / run

```bash
bash additional_references/jxm35_adapter/build.sh
./harness --engine jxm35_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into
`third_party/jxm35_limit_order_book_matching_engine/` at the pinned commit
and patches the cloned `OrderBook.cpp` in place. Use
`ME_JXM35_SRC=/path/to/checkout` to skip the clone (the script still applies
the patch to your checkout — `git reset --hard` first if you want to undo).
