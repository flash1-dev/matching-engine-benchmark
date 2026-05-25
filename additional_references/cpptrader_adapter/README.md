# cpptrader_adapter — integration example

Wraps [chronoxor/CppTrader](https://github.com/chronoxor/CppTrader) behind
`api/matching_engine_api.h`. The engine depends on
[chronoxor/CppCommon](https://github.com/chronoxor/CppCommon) for its
container, allocator, and utility headers.

Pinned commits:
- `chronoxor/CppTrader`  — `831d10e2a6dd96ac7b063f1d418f6563cbf74c50`
- `chronoxor/CppCommon`  — `e14011974b8d463cc854239bf351275b5a857de6`

This adapter is one of five worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`MarketManager` facade + `MarketHandler` callback interface, single-threaded
matcher. Native APIs visible from the adapter:

- `MarketManager::AddSymbol(Symbol)` / `AddOrderBook(Symbol)` — one-time
  per-symbol setup.
- `MarketManager::EnableMatching()` — auto-matching is OFF by default; the
  adapter enables it once after the order book is created.
- `MarketManager::AddOrder(Order)` — matches inline; for IOC, residual is
  discarded by the engine (no rest).
- `MarketManager::ModifyOrder(id, new_price, new_quantity)` — pulls the
  order off the book, reprices, runs `MatchLimit`, re-adds any leftover.
- `MarketManager::DeleteOrder(id)` — synchronous.
- `MarketManager::GetOrder(id)` — `const Order*`, `nullptr` if not resting.
- `OrderBook::best_bid()` / `best_ask()` — `const LevelNode*`.
- `OrderBook::GetBid(price)` / `GetAsk(price)` — `const LevelNode*` with
  `TotalVolume`.
- `MarketHandler::onExecuteOrder(order, price, qty)` — called inside the
  matching loop, twice per fill: once with the maker (resting) order, then
  once with the taker (incoming) order.
- `Order::BuyLimit(...)` / `SellLimit(...)` factory functions accept a
  `OrderTimeInForce` (`GTC`/`IOC`/`FOK`/`AON`).

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports
matching the harness's wire format. `Symbol::Name` is a fixed `char[8]` that
the constructor `memcpy`s 8 bytes from — pad short names to 8 chars (the
adapter uses `WORKLD\0\0`).

## Adapter strategy

- A `HarnessHandler : MarketHandler` pairs consecutive `onExecuteOrder`
  callbacks (first call = maker, second = taker) into one harness Trade
  report and tallies the taker's filled quantity for the IOC residual.
  Other `MarketHandler` hooks are intentional no-ops.
- **OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject** are
  synthesised above the engine. CancelAck reads the resting price/qty from
  `g_manager->GetOrder()` BEFORE the `DeleteOrder` call so the report
  carries the truth at the moment of cancel.
- **IOC** delegated to the engine via `OrderTimeInForce::IOC`. The adapter
  emits `CancelAck` for the residual when the taker's filled tally falls
  short of the input quantity.
- **Modify** dispatched to the engine's native `ModifyOrder` — the engine
  itself does cancel + reprice + match + re-add, which lines up with the
  harness's "cancel + reinsert, losing queue priority" contract. Crossing
  fills flow through the same `onExecuteOrder` path and inherit the modify
  message's seq via the adapter's per-call context.
- Shadow map (`oid -> {side, price, alive}`) drives the reject path and
  echoes side on CancelAck/ModifyAck. Liveness is double-checked against
  `g_manager->GetOrder()` so a "live in shadow but fully filled by an
  earlier crossing" race rejects correctly.

## Build / run

```bash
bash additional_references/cpptrader_adapter/build.sh
./harness --engine cpptrader_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones CppTrader into `third_party/CppTrader/` and CppCommon
into `third_party/CppTrader/modules/CppCommon/` at the pinned commits.
The upstream uses a `gil` package manager to fetch CppCommon; we sidestep
`gil` and clone CppCommon directly to the path the upstream CMake expects.
Overrides: `ME_CPPTRADER_SRC` and `ME_CPPCOMMON_SRC` use existing
checkouts in place of cloning.
