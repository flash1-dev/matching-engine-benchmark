# darkpool_adapter — integration example

Wraps [dendisuhubdy/dark_pool](https://github.com/dendisuhubdy/dark_pool) behind
`api/matching_engine_api.h`. The engine that actually compiles and runs in
dark_pool is its QuickFIX-style `ordermatch` sample book in `src/ordermatch/`
(`Market.cpp` + `Order.h`): a price/time-priority limit order book over two
`std::multimap`s, driven by the `Market` class. The repo's README mentions
Liquibook, but Liquibook is not what builds here.

Pinned commit:
- `dendisuhubdy/dark_pool` — `92bc3382bda9375829a2267ac3e96a80802b60cf`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

`Market` (`src/ordermatch/Market.{h,cpp}`), single-threaded matcher, matches
synchronously on the calling thread. The book is two `std::multimap`s keyed by
price (`m_bidOrders` descending, `m_askOrders` ascending); each value is a
QuickFIX `Order` (`src/ordermatch/Order.h`). Orders carry a string `clientID`
(the engine's own ClOrdID). Native APIs visible from the adapter:

- `Market::insert(Order)` — rest / present an incoming order in the book.
- `Market::match(std::queue<Order>&)` — the engine's own crossing loop. For each
  fill it pushes the two filled `Order` copies, **bid then ask**, into the queue;
  it stamps `getLastExecutedPrice()` / `getLastExecutedQuantity()` on each copy
  and removes any order it closes from the book.
- `Market::find(Side, clientID)` — locate a resting order by `(side, id)`.
- `Market::erase(Order)` — remove a resting order (native cancel).
- `Order::getOpenQuantity()` / `Order::isClosed()` — remaining qty / fully-filled.

Not provided natively: no IOC / FOK / POST-ONLY; no native modify; no result code
that says an order is not resting (so cancel/modify of an absent order cannot be
adjudicated by the engine); `find`/`erase` are keyed by `(side, id)` while the
harness cancel/modify carry only `order_id`; no best-bid/ask or depth accessor;
no ack/reject events.

## Adapter strategy

- **No matching is reimplemented in the adapter.** Every fill, and every executed
  price, comes from `Market::match`. After each `insert` (new order, or the
  reinsert leg of a modify) the adapter drains the engine's fill queue and emits
  one `ME_TRADE` per crossing.
- **Trades / maker price.** Per the contract, `Trade.price_ticks` is the maker's
  (resting) price and `maker/taker_order_id` name the two orders. The engine
  pushes the bid and ask copies of each fill; the aggressor is the order just
  inserted, so the **maker is whichever copy is *not* the aggressor**. The
  adapter prints that maker copy's engine-computed `getLastExecutedPrice()`
  (which the source patch below makes correct for the resting-bid-vs-incoming-sell
  case) and the per-fill `getLastExecutedQuantity()`.
- **Order-id mapping.** Harness `uint64_t` ids are marshalled to/from the engine's
  decimal-string `clientID` (`std::to_string` / `std::strtoull`) — exactly as the
  engine's own FIX front-end carries a ClOrdID.
- **Prices** are passed straight through: workload `int64_t` ticks → the engine's
  `double` price and back (`static_cast`). The canonical workload's prices are
  small integers, so the round-trip is exact.
- **IOC**: the engine has no IOC flag, so the order is submitted as a plain limit,
  matches what it can, and the adapter then `erase`s the rested residual and emits
  one `CancelAck` for it, so an IOC order never rests. A fully-filled IOC emits no
  CancelAck.
- **Modify**: no native modify — explicit `erase` + re-`insert` at the new
  price/quantity (the reinsert loses queue priority; its crossing fills carry the
  modify message's `sequence_number`, per the harness contract).
- A per-order **liveness shadow** — a flat `std::vector<Shadow>` indexed by the
  dense harness order id, pre-sized to `2^16` and grow-only — holds
  `{price, side, live}`. It is required because the engine exposes no "not
  resting" result code (so the adapter can synthesise `CancelReject` /
  `ModifyReject` for an order that is already filled / already cancelled / never
  seen — the canonical workload injects ~2% stale cancels/modifies) and because
  `find`/`erase` need the order's *side* (the harness cancel/modify carry only the
  id). `OrderAck` / `CancelAck` / `ModifyAck` / `CancelReject` / `ModifyReject` are
  synthesised above the engine, which has no ack/reject callback.
- **Audit queries.** The engine has no best-bid/ask or depth accessor, so
  `engine_query_best_bid/ask` scan the live shadow for the extreme resting price
  per side, and `engine_query_depth_at` sums the engine's own
  `Order::getOpenQuantity()` over the live orders the shadow locates at that
  `(price, side)` — the *quantity* always comes from the engine, never from the
  shadow. The state audit (192 checks vs the `liquibook` baseline) passes, so the
  shadow's liveness set tracks the engine's book exactly.

The matcher runs synchronously, so `engine_flush` is a no-op and **no
`engine_prebuild` is exported** — there is no translation-only prebuild step; all
work happens in the `engine_on_*` handlers.

## Source patch

`build.sh` applies **one** engine correctness patch, after `git reset --hard` to
the pin so the reset can never clobber it (idempotent; fails loud if its anchors
drift). This adapter is classified **"with fix"**: the patch is applied
unconditionally, so `darkpool_adapter.so` is the fixed engine.

- **Maker-priced fill** (`src/ordermatch/Market.{h,cpp}`). Filed upstream as
  [dendisuhubdy/dark_pool#1](https://github.com/dendisuhubdy/dark_pool/issues/1).
  `Market::match(Order& bid, Order& ask)` prices every fill unconditionally at the
  ask (`double price = ask.getPrice();`, `Market.cpp:109`), so when a resting BUY
  sits above an incoming SELL the print is the aggressor's lower price, not the
  resting maker's. Under price-time priority the resting (maker) order sets the
  execution price. The fix records the aggressor's side in `Market::insert()`
  (`m_lastInsertedSide` — `insert` is the only order presented before each
  `match` pass) and prices the inner `match()` at the side opposite the aggressor:
  `ask.getPrice()` when the aggressor bought (ask is the maker), `bid.getPrice()`
  when the aggressor sold (bid is the maker). The public API is unchanged
  (`match(queue&)` keeps its signature — a pure engine-internal correction), and
  the already-correct aggressive-buy path is a no-op. Quantities and book state
  are untouched.

  The patch is **necessary and sufficient** for `normal`: built against the
  unmodified engine the harness reports `Status: FAIL` (the trade-price hash
  diverges — the state audit still passes, because only the *printed price* is
  wrong, not the book); with the patch the hash matches and the verdict is
  `VALID`. The first baseline divergence on `normal` is a sell crossing a higher
  resting bid (same qty/maker/taker, different price).

## Build / run

```bash
bash additional_references/darkpool_adapter/build.sh
./harness --engine darkpool_adapter.so --scenario normal --mode audit \
          --matcher-core 60 --drainer-core 61
```

`build.sh` clones the engine into `third_party/dendisuhubdy_dark_pool/` at the
pinned commit, applies the maker-price patch, and compiles
`src/ordermatch/Market.cpp` + this adapter into `darkpool_adapter.so` at the repo
root with the system `g++` (C++17). Override:
`ME_DARKPOOL_SRC=/path/to/existing/clone` uses an existing checkout in place of
cloning (the patch is re-applied idempotently).
