# brprojects_adapter — integration example

Wraps [brprojects/Limit-Order-Book](https://github.com/brprojects/Limit-Order-Book)
(a "Central Limit Order Book") behind `api/matching_engine_api.h`. The engine is
a price-time-priority limit order book in C++: each side is an AVL tree of price
levels (`Limit`), each level a FIFO doubly-linked list of `Order` nodes, plus an
`order_id -> Order*` hash map for cancel-by-id.

Pinned commit:
- `brprojects/Limit-Order-Book` — `af6e5349874649fe196bd6c26653d357f5a751f2`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`Book` (`Limit_Order_Book/Book.hpp`), single-threaded matcher, matches
synchronously on the calling thread. A new limit order is first crossed against
the opposite book (`limitOrderAsMarketOrder` → `marketOrderHelper`) and the
residual rests. Native APIs visible from the adapter:

- `Book::addLimitOrder(int orderId, bool buyOrSell, int shares, int limitPrice)`
  — inserts and matches internally (crosses, then rests the residual).
- `Book::cancelLimitOrder(int orderId)` — unlinks a resting order by id;
  silently no-ops an id that is not resting.
- `Book::modifyLimitOrder(int orderId, int newShares, int newLimit)` —
  re-appends WITHOUT re-matching (see Adapter strategy — not used).
- `Book::getHighestBuy()` / `getLowestSell()` / `getBuyTree()` / `getSellTree()`
  — `Limit*` accessors into the AVL trees; `Limit::getLimitPrice()`,
  `Limit::getHeadOrder()`, `Order::getOrderId()`, `Order::getShares()`.
- market / stop / stop-limit variants the canonical workload never uses.

Order ids and prices are `int` (32-bit signed) throughout the engine.

Not provided natively: no IOC / FOK / POST-ONLY flags; no Ack/Reject report
types of any kind; **no trade/fill notification whatsoever** — `marketOrderHelper`
executes and deletes resting orders silently and only bumps an int counter (see
the source patch); no by-level depth query (only the raw AVL trees).

## Adapter strategy

- **Trades** are surfaced by the injected per-fill hook (see "Source patch"):
  the hook fires once per fill with the taker (aggressor) id, the maker
  (resting) id, the maker's resting price, and the fill quantity, and the
  adapter emits one `ME_TRADE` per call (maker price = the resting level's
  price; maker/taker ids from the hook; the aggressor's `sequence_number`
  threaded through a per-call context set just before the `addLimitOrder` call).
  The hook also accumulates the taker's filled tally (drives the IOC residual)
  and decrements the maker's shadow. Fills arrive in match order (best price
  first, FIFO within a level), exactly as the harness wants.
- **Prices / ids**: the canonical workload's ids are dense `1..N` (≤ ~300k) and
  prices are small positive ticks (mid ~33.5k, depth ≤ 799 ticks out), so both
  fit `int` with vast headroom; the adapter passes the 64-bit ABI values through
  `int()` and keeps its own 64-bit shadow for reporting.
- **IOC**: the engine has no IOC flag, so the order is submitted as a plain
  limit, matches what it can, and the adapter then cancels the rested residual
  (via the engine's native `cancelLimitOrder`) and emits one `CancelAck` for it,
  so an IOC order never rests.
- **Modify**: the engine's own `modifyLimitOrder` re-appends WITHOUT
  re-matching, which violates the harness's "modify loses priority and may
  re-cross" contract, so it is **not** used. The adapter does the modify as an
  explicit `cancelLimitOrder` + `addLimitOrder` at the new price/quantity, so the
  reinsert re-crosses the book and its crossing fills are reported and carry the
  modify message's `sequence_number`.
- A per-order **shadow** (a flat array indexed by the dense harness order id;
  grow-only, pre-sized to 2^19 entries) holds `{price, side, remaining, alive}`.
  It is required to adjudicate cancel/modify of a not-resting order (the engine's
  `cancelLimitOrder` silently no-ops a not-resting id, so the adapter must decide
  "is this order resting?" itself → `CancelReject` / `ModifyReject`; the
  canonical workload injects stale cancels/modifies) and to echo the resting
  order's side/price/qty on the ack (the engine surfaces none of that on cancel).
  The shadow is kept in lockstep with the engine's own `orderMap`/AVL trees by
  the fill hook (decrement maker remaining) and the new/cancel/modify handlers.

`OrderAck` / `CancelAck` / `ModifyAck` / `CancelReject` / `ModifyReject` are
synthesised above the engine, which has no ack/reject callback. The matcher runs
synchronously, so `engine_flush` is a no-op and no `engine_prebuild` is exported
(there is no translation-only prebuild step; all work happens in the
`engine_on_*` handlers). `engine_query_best_bid` / `best_ask` / `depth_at` scan
the per-order shadow, which the fill hook and handlers hold equal to the engine's
live book; the engine exposes only its raw AVL trees (no aggregate-by-level
query), and the shadow is the report/audit mirror of those trees.

## Source patch

`build.sh` applies **one** source patch to the engine, after `git reset --hard`
to the pin so the reset can never clobber it (idempotent; fails loud if an
anchor drifts):

- **Per-fill trade hook** (adapter instrumentation, `Limit_Order_Book/Book.cpp`).
  The engine ships with no trade/fill callback of any kind: the match loop
  `Book::marketOrderHelper` executes and deletes resting orders silently and only
  bumps an int counter. The harness needs one `ME_TRADE` per fill carrying the
  maker's resting price + maker/taker ids — matcher information only the engine
  sees — and re-deriving fills in the adapter would mean reimplementing matching
  (forbidden by the adapter mandate). The patch forward-declares
  `extern "C" void (*g_brp_fill_hook)(int taker_id, int maker_id, int
  maker_price, int qty)` and inserts one call to it at each of
  `marketOrderHelper`'s two fill sites — the fully-consumed-maker loop and the
  partial-fill tail — reading the maker id / maker price / fill qty **before** the
  engine mutates the order. The adapter implements the hook (set in
  `engine_init`, cleared in `engine_shutdown`); when the pointer is null (the
  engine's own standalone `main`) every call site is a null-checked no-op, so the
  patch is inert outside the harness. It adds only those two emit points; the
  matching logic, prices, and quantities are otherwise byte-identical to the
  pinned source. This is the same "a hook the engine should call but doesn't"
  pattern used by `jxm35_adapter` and `kautenja_adapter`.

This patch is adapter instrumentation, **not** a correctness fix. The harness
classifies brprojects as conforming **as shipped** — `CORRECTNESS_FINDINGS.md`
records "No fix required" (an uncached-height AVL; the matching, cancel, and
modify logic are consensus-conforming as written), so there is no engine defect
to patch and no upstream issue to cite. `build.sh` therefore changes nothing in
the matching logic — it only surfaces the per-fill stream the engine never
exposes.

## Build / run

```bash
bash additional_references/brprojects_adapter/build.sh
./harness --engine brprojects_adapter.so --scenario normal --mode audit \
          --matcher-core 52 --drainer-core 53
```

`build.sh` clones the engine into `third_party/brprojects_limit_order_book/` at
the pinned commit (the upstream repo is the engine — the matcher sources are
under its `Limit_Order_Book/` directory; no submodules), applies the trade-hook
patch, and compiles the three matcher translation units (`Book.cpp`, `Limit.cpp`,
`Order.cpp`) + this adapter into `brprojects_adapter.so` at the repo root with the
system `g++` (C++20). Override: `ME_BRPROJECTS_SRC=/path/to/existing/clone` uses
an existing checkout in place of cloning (the patch is re-applied idempotently).
