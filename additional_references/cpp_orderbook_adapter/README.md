# cpp_orderbook_adapter — integration example

Wraps [geseq/cpp-orderbook](https://github.com/geseq/cpp-orderbook) behind
`api/matching_engine_api.h`. The engine depends on
[geseq/cpp-decimal](https://github.com/geseq/cpp-decimal) for its fixed-point
price/quantity type and [geseq/cpp-pool](https://github.com/geseq/cpp-pool)
for its order-node allocator, plus Boost intrusive containers.

Pinned commits:
- `geseq/cpp-orderbook` — `b58d931b02928a83b4038fa2125edce14adbd90e`
- `geseq/cpp-decimal`   — `88646b353a4ef191b4936bf765554c726dcaf9fb` (tag `v2.1.0`)
- `geseq/cpp-pool`      — `730fe13f2c473b8ef4fe73c58dad048016c1fffd` (tag `v0.5.0`)

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `discoveries.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Header-template C++ limit-order book, single-threaded matcher. Native APIs
visible from the adapter:

- `OrderBook<Notification>(notification, ...)` — constructed with the
  notification sink and initial pool sizes; the order-node pool grows on
  demand.
- `OrderBook::addOrder(id, Type::Limit, Side, qty, price, Flag)` — matches
  inline; `Flag::IoC` requests immediate-or-cancel (residual discarded by the
  engine, no rest, no notification).
- `OrderBook::cancelOrder(id)` — synchronous; reports `Canceled` on removal or
  `Rejected(OrderNotExists)` for a never-seen / already-terminal id.
- `OrderBook::hasOrder(id)` — liveness probe.
- `NotificationInterface<Implementation>::onExecutionReport(ExecutionReport)`
  — the single CRTP callback the engine fires per event (`New` / `Trade` /
  `Canceled` / `Rejected`). Non-virtual static dispatch the compiler inlines.
  `Trade` carries `maker_order_id` / `taker_order_id` / `last_qty` /
  `last_price`.
- `Decimal` (= `decimal::U8`) — fixed-point price/quantity; `Decimal(i, 0)`
  and `d.to_int()` round-trip an integer tick.

The engine has no native modify, and its `Trade` / `Canceled` reports carry
neither the order side nor the price the harness wire format requires.
Internally per-fill notifications are routed through `std::function`
(`pricelevel.cpp` is explicitly instantiated against the
`TradeNotification` / `PostOrderFill` aliases) — that indirection is
engine-level and not removable from the adapter.

## Adapter strategy

- A `HarnessNotification : NotificationInterface<HarnessNotification>` turns
  each `Trade` into one harness Trade report (maker price = resting
  `last_price`, aggressor seq threaded through the per-call context) and
  tallies the taker's filled quantity for the IOC residual. The `Canceled` /
  `Rejected` callbacks record the engine's cancel verdict.
- **OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject** are
  synthesised above the engine, since the engine's reports lack the side/price
  the harness needs. The engine's own `New`/`Accepted` notification is ignored;
  `engine_on_new_order` emits the `OrderAck` eagerly.
- **IOC** delegated to the engine via `Flag::IoC`. The engine discards the
  unfilled residual without notifying, so the adapter emits the residual
  `CancelAck` itself when the taker's filled tally falls short of the input
  quantity.
- **Modify** is cancel + reinsert: the engine has no native modify, so the
  adapter calls `cancelOrder` (the engine adjudicates the cancel half — a
  `Rejected` becomes a `ModifyReject`) then re-adds the order at the new
  price/quantity. The reinsert loses queue priority and its crossing fills
  cross with the modify message's seq, matching the harness contract.
- A per-order **shadow** — a flat array indexed by order id holding
  `{price, side, remaining, alive}` — supplies the side/price echoed on the
  synthesised acks and is the single source of truth for the audit queries
  (`engine_query_best_bid` / `best_ask` / `depth_at` scan it).
- **Decimals**: workload `price_ticks` (int64) and `quantity` (uint32) map to
  `Decimal(value, 0)`, whose internal fp = `value * 10^8` preserves a strictly
  increasing, bit-for-bit tick ordering; `Decimal::to_int()` recovers the tick
  exactly.

## Build / run

```bash
bash additional_references/cpp_orderbook_adapter/build.sh
./harness --engine cpp_orderbook_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` clones cpp-orderbook into `third_party/cpp_orderbook/`,
cpp-decimal into `third_party/cpp_decimal/`, and cpp-pool into
`third_party/cpp_pool/` at the pinned commits, then compiles the four engine
matching translation units (`pricelevel.cpp`, `orderqueue.cpp`, `order.cpp`,
`types.cpp`) together with this adapter into a single `.so` at the repo root.
Boost is taken from system headers by default (the harness itself requires
`libboost-all-dev`). Overrides: `ME_CPP_ORDERBOOK_SRC`, `ME_DECIMAL_SRC`, and
`ME_POOL_SRC` use existing checkouts in place of cloning; `ME_BOOST_SRC`
points at a Boost source tree or CPM superproject root for boxes without
system Boost.

## Engine issue (resolved upstream)

cpp-orderbook had a price-cross defect — under certain crossing sequences the
book could match across the spread incorrectly — which has since been fixed
upstream. The pinned commit
[`b58d931`](https://github.com/geseq/cpp-orderbook/commit/b58d931b02928a83b4038fa2125edce14adbd90e)
is `main` HEAD and already contains the fix, so the pin needs no patch:
`build.sh` clones and resets without a `sed` step. Full mechanics, provenance,
and the upstream resolution are in `../../RESOLVED_FINDINGS.md`.
