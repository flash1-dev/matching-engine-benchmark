# tzadiko_adapter — integration example

Wraps [Tzadiko/Orderbook](https://github.com/Tzadiko/Orderbook) behind
`api/matching_engine_api.h`.

Pinned commit: `dd136dd219ead95796f0e396e9e1395542bf673f`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
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
  synthesised above the engine. A shadow map `oid -> {price, side,
  remaining, alive}` drives the reject path and the CancelAck/ModifyAck
  side+price echo. The map is also updated from the fill stream so a
  fully-filled maker is recorded as not-resting on the next
  cancel/modify.

- **IOC**: `OrderType::FillAndKill`. The engine cancels any residual at the
  tail of `MatchOrders` itself; the adapter detects `residual =
  quantity - filled` after the call and emits the harness's CancelAck for
  the unfilled remainder (no second cancel into the engine).

- **Modify**: the harness contract is cancel + reinsert with the new
  price/qty, and the crossing trades carrying the modify message's seq.
  The adapter does this explicitly (`CancelOrder` + `AddOrder`) rather
  than calling the engine's `ModifyOrder`, so the adapter has full
  control over the trade-seq attribution.

- **Audit queries** (`best_bid`, `best_ask`, `depth_at`) are answered from
  an adapter-side shadow (`std::map<int64_t, uint64_t>` per side) updated
  from the same fill / ack / cancel stream the adapter already maintains
  for the reject path. The engine's native `GetOrderInfos()` walks every
  resting order to aggregate per-level totals (O(N_resting) per call);
  bypassing it keeps the 192 audit probes cheap.

## Build patches

`build.sh` applies two source-level patches to `Orderbook.cpp`. Both are
idempotent (the `git reset --hard` step restores the file before each
rerun).

1. **`localtime_s` -> `localtime_r`** — `localtime_s` is Win32-only; the
   POSIX equivalent takes the arguments in the opposite order. The call
   sits inside `PruneGoodForDayOrders`, a background thread that sleeps
   until 16:00 local time and therefore never wakes during the run.

2. **Drop `trades.reserve(orders_.size())` from `MatchOrders`** — removes
   a per-match O(orders_) allocation hint with no semantic effect on the
   output stream. See `discoveries.md` for context.

## Build / run

```bash
bash additional_references/tzadiko_adapter/build.sh
./harness --engine tzadiko_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones the engine into `third_party/Tzadiko_Orderbook/` at the
pinned commit and applies the two patches above. Use
`ME_TZADIKO_SRC=/path/to/checkout` to skip the clone (the script still
applies the patches to your checkout — `git reset --hard` first if you
want to undo).
