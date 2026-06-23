# mansoor_adapter — integration example

Wraps [mansoor-mamnoon/limit-order-book](https://github.com/mansoor-mamnoon/limit-order-book)
behind `api/matching_engine_api.h`.

Pinned commit: `78e1fb0e0563388456e5030d858ef43d6407bed3`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`BookCore` + `IPriceLevels` (`PriceLevelsContig` / `PriceLevelsSparse`). The
adapter uses `PriceLevelsContig`. Native APIs visible from the adapter:

- `BookCore::submit_limit(NewOrder)` — synchronous, returns `ExecResult{
  filled, remaining }`.
- `BookCore::cancel(OrderId)` — returns `bool` (true = found and cancelled).
- `BookCore::modify(ModifyOrder)` — native modify: internally erase +
  cross-match at the new price + tail-enqueue the remainder (i.e. exactly
  the harness's cancel + reinsert contract); returns `ExecResult{0, 0}` for
  an unknown id. The adapter uses it directly (see *Adapter strategy*).
- `IEventLogger` callback interface with `log_new` / `log_fill` / `log_cancel`
  and `on_book_after_event` hooks.
- `OrderFlags { IOC, FOK, POST_ONLY, STP }` — native IOC.

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports
matching the harness's wire format; no echo of side/price on cancel.

## Adapter strategy

- A `HarnessLogger : IEventLogger` captures every `log_fill` into a Trade
  report. The seq comes from a global `g_cur_seq` the adapter sets before
  each engine call.
- **IOC** delegated to the engine via `NewOrder.flags |= IOC`; the adapter
  emits the harness's `CancelAck` for the residual using
  `o->quantity - r.filled`.
- **Modify** calls the engine's native `modify()` directly — it *is* the
  harness contract (erase + cross-match + tail-enqueue), with crossing fills
  stamped by the modify's seq via `g_cur_seq`.
- **Rejects** are adjudicated by the engine itself: `cancel()`'s `bool` and
  `modify()`'s `ExecResult{0, 0}` (not-found) decide CancelReject /
  ModifyReject. The engine's id index erases fully-filled makers at fill
  time, so cancels of terminal orders reject correctly with no adapter-side
  liveness tracking.
- A per-order shadow (a flat vector indexed by the dense harness order id,
  `oid -> {price, side}`) echoes side/price on CancelAck/ModifyAck — the one
  payload the engine's API does not return.

## Source patch

mansoor is **with fix**: `build.sh` applies one engine-source correctness patch
(after `git reset --hard` to the pin, idempotent + anchor-checked) before
compiling. As shipped, `PriceLevelsContig` allocates one `LevelFIFO` per tick in a
fixed `[min_tick, max_tick]` band and `get_level()` / `has_level()` return
`&levels_[idx(px)]` with an **unchecked** index, so a limit priced outside the band
indexes `levels_` out of bounds and corrupts the heap / SIGSEGVs — mansoor
**aborts on wide-swing scenarios** (e.g. `flash-crash`) as shipped. The patch
bounds-checks the index and routes an out-of-domain price to an isolated sentinel
level, so the order drops cleanly (never scanned in-band, never best-of-book, zero
depth) instead of crashing. Filed upstream as
[mansoor-mamnoon/limit-order-book#3](https://github.com/mansoor-mamnoon/limit-order-book/issues/3);
see [`../../CORRECTNESS_FINDINGS.md`](../../CORRECTNESS_FINDINGS.md) (verdict "Fix
submitted upstream … VALID ×5 across 100 seeds").

## Build / run

```bash
bash additional_references/mansoor_adapter/build.sh
./harness --engine mansoor_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/mansoor_limit_order_book/` at
the pinned commit. Use `ME_MANSOOR_SRC=/path/to/checkout` to skip the clone.
