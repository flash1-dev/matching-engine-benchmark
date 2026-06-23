# tzadiko_adapter — integration example

Wraps [Tzadiko/Orderbook](https://github.com/Tzadiko/Orderbook) behind
`api/matching_engine_api.h`.

Pinned commit: `dd136dd219ead95796f0e396e9e1395542bf673f`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Single-symbol `Orderbook`. Native APIs visible from the adapter:

- `Orderbook::AddOrder(OrderPointer)` — returns `Trades`, a vector of
  `Trade { TradeInfo bid, TradeInfo ask }` pairs (one entry per fill).
- `Orderbook::CancelOrder(OrderId)` — `void`, silent no-op when the order
  is not present.
- `Orderbook::ModifyOrder(OrderModify)` — engine-internal cancel + AddOrder
  at the new price/qty; returns the trades the re-add crosses.
- `Orderbook::Size()` and `Orderbook::GetOrderInfos()` for inspection (the
  latter is an O(N) snapshot of bid+ask levels with aggregated quantity).
- `OrderType::{ GoodTillCancel, FillAndKill, FillOrKill, GoodForDay,
  Market }` — `FillAndKill` is the closest equivalent to IOC.

Storage layout: `std::map<Price, std::list<OrderPointer>, std::greater<>>`
for bids and the symmetric `std::less<>` map for asks, indexed by an
`std::unordered_map<OrderId, OrderEntry>` for cancel lookup. Order objects
are `std::shared_ptr<Order>`. Every public method takes a member
`std::mutex`.

Not provided natively: no `best_bid` / `best_ask` accessor (we drive
`GetOrderInfos()` for those audit queries); no Ack / Reject report types;
no per-fill callback (trades returned by value).

## Adapter strategy

- `AddOrder` returns the trade list by value; the adapter iterates it and
  emits one `ME_TRADE` per entry. The aggressor's side is known from the
  harness call — the maker is the opposite-side `TradeInfo`, and the
  maker's price is what the harness records in `ME_TRADE.price_ticks`.

- **OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject** are
  synthesised above the engine. A per-order shadow `oid -> {price, side,
  remaining, alive}` — a flat vector indexed by the dense 1-based harness
  order id, sized in `engine_prebuild` — drives the reject path and the
  CancelAck/ModifyAck side+price echo. The shadow state is necessary: the
  engine's `CancelOrder` returns `void` (silent on unknown ids),
  `ModifyOrder` returns `{}` ambiguously, and no API exposes a resting
  order's fields. It is updated from the fill stream so a fully-filled
  maker is recorded as not-resting on the next cancel/modify.

- **IOC**: `OrderType::FillAndKill`, the engine's own IOC type. The engine
  cancels any residual at the tail of `MatchOrders` itself — which is
  exactly the path that self-deadlocks as shipped and that `build.sh`'s
  third patch fixes (below). The adapter detects `residual = quantity -
  filled` after the call and emits the harness's CancelAck for the unfilled
  remainder (no second cancel into the engine).

- **Modify**: the engine's native `ModifyOrder` — itself cancel + `AddOrder`
  at the new price/qty, which is exactly the harness contract — with the
  crossing trades stamped with the modify message's seq. The shadow gate
  stays in front of it only because the engine cannot adjudicate: a
  `ModifyOrder` on an unknown id returns `{}`, indistinguishable from a
  successful non-crossing modify.

- **Audit queries** (`best_bid`, `best_ask`, `depth_at`) are answered from
  the engine's native `GetOrderInfos()` level snapshot. The snapshot walk is
  O(N_resting) per call, but the harness excludes probe time from the timed
  total, so the engine's own answer costs the measurement nothing — where a
  parallel adapter-side level map would cost sorted-map maintenance on every
  timed message.

## Build patches

`build.sh` applies three source-level patches to `Orderbook.cpp`. All are
idempotent (the `git reset --hard` step restores the file before each
rerun).

1. **`localtime_s` -> `localtime_r`** — `localtime_s` is Win32-only; the
   POSIX equivalent takes the arguments in the opposite order. The call
   sits inside `PruneGoodForDayOrders`, a background thread that sleeps
   until 16:00 local time.

2. **Drop `trades.reserve(orders_.size())` from `MatchOrders`** — removes
   a per-match O(orders_) allocation hint with no semantic effect on the
   output stream. See `CORRECTNESS_FINDINGS.md` for context.

3. **`CancelOrder` -> `CancelOrderInternal` at the two `MatchOrders` tail
   sites** — a correctness fix, not a performance one. The tail cancels a
   partially-filled FillAndKill residual by calling the *public*
   `CancelOrder` while `AddOrder` still holds the book's non-recursive
   `ordersMutex_` — a guaranteed self-deadlock the first time a FillAndKill
   order partially fills, so the engine's own IOC type cannot execute as
   shipped. The engine already provides the already-locked variant
   (`CancelOrderInternal`, what its bulk `CancelOrders` uses under its own
   lock); the patch switches the only two locked-context callers to it.
   Trades and end-of-call book state are identical to what an un-deadlocked
   public cancel would produce. See the filed upstream issues
   [#11](https://github.com/Tzadiko/Orderbook/issues/11) and
   [#12](https://github.com/Tzadiko/Orderbook/issues/12) for the full account
   (including the correction of this engine's earlier `infeasible` verdict).

## Build / run

```bash
bash additional_references/tzadiko_adapter/build.sh
./harness --engine tzadiko_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/Tzadiko_Orderbook/` at the
pinned commit and applies the three patches above. Use
`ME_TZADIKO_SRC=/path/to/checkout` to skip the clone (the script still
applies the patches to your checkout — `git reset --hard` first if you
want to undo).
