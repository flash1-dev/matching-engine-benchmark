# mansoor_adapter — integration example

Wraps [mansoor-mamnoon/limit-order-book](https://github.com/mansoor-mamnoon/limit-order-book)
behind `api/matching_engine_api.h`.

Pinned commit: `78e1fb0e0563388456e5030d858ef43d6407bed3`.

This adapter is one of eight worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`BookCore` + `IPriceLevels` (`PriceLevelsContig` / `PriceLevelsSparse`). The
adapter uses `PriceLevelsContig`. Native APIs visible from the adapter:

- `BookCore::submit_limit(NewOrder)` — synchronous, returns `ExecResult{
  filled, remaining }`.
- `BookCore::cancel(OrderId)` — returns `bool` (true = found and cancelled).
- `BookCore::modify(ModifyOrder)` — native modify; the adapter does NOT use it
  (see *Adapter strategy* below).
- `IEventLogger` callback interface with `log_new` / `log_fill` / `log_cancel`
  and `on_book_after_event` hooks.
- `OrderFlags { IOC, FOK, POST_ONLY, STP }` — native IOC.

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports
matching the harness's wire format; no echo of side/price on cancel.

## Adapter strategy

- A `HarnessLogger : IEventLogger` captures every `log_fill` into a Trade
  report. The seq comes from a thread-local `g_cur_seq` the adapter sets
  before each `submit_limit` call.
- **IOC** delegated to the engine via `NewOrder.flags |= IOC`; the adapter
  emits the harness's `CancelAck` for the residual using
  `o->quantity - r.filled`.
- **Modify** uses the harness contract (cancel + reinsert) rather than the
  engine's native in-place `modify()` — the latter would not carry the
  modify's seq through to crossing fills the way the harness expects.
- Shadow map for the reject path and CancelAck side/price echo.

## Build / run

```bash
bash additional_references/mansoor_adapter/build.sh
./harness --engine mansoor_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/mansoor_limit_order_book/` at
the pinned commit. Use `ME_MANSOOR_SRC=/path/to/checkout` to skip the clone.
