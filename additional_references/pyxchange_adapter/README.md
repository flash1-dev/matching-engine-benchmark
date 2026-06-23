# pyxchange_adapter — integration example

Wraps [pavelschon/PyXchange](https://github.com/pavelschon/PyXchange) behind
`api/matching_engine_api.h`. PyXchange is a limit-orderbook matching engine
whose **core is C++** (a `boost::multi_index` price-time book) with a
Boost.Python / Twisted server layer on top. This adapter drives the C++ matcher
core directly and ignores the Python/Twisted layer entirely.

Pinned commit:
- `pavelschon/PyXchange` — `b35f0ebeb8ce008e605987305a2d52194785fbb8`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`pyxchange::OrderBook` (`src/orderbook/`), single-threaded matcher, matches
synchronously on the calling thread. The book is a pair of `OrderContainer`s
(one per side), each a `boost::multi_index_container<OrderPtr>` with three
indices (`src/order_container/OrderContainer.hpp`):

- `ordered_unique` on a composite **(price, time)** key — the match-walk order
  (price priority, then FIFO by `time`);
- `ordered_non_unique` on **price** — price-level / depth iteration;
- `hashed_unique` on **(trader, orderId)** — cancel / lookup by id.

The match loop is `OrderBook::handleExecution` / `insertOrder` /
`cancelOrder<>` (`src/orderbook/OrderBook*.cpp`): a crossing order is matched
against the opposite container, fills print at the **resting maker's** price,
and the residual is inserted to rest. These — and the `OrderContainer` data
structure — are used **unmodified**.

Native API the engine exposes to its own server layer is `py::dict`-based
(`OrderBook::createOrder` / `marketOrder` / `cancelOrder(trader, decoded)`),
decoded from JSON, and reporting is a Python per-trader / per-client notify
fan-out. Not provided in a non-Python-callable form: no plain-typed C++ submit
entry point; no IOC / FOK / POST-ONLY flag; no native modify; no ack / reject
events (the engine notifies a Python client object); order identity is
**(trader, orderId)**, not a bare `uint64_t`.

## Adapter strategy

- **One synthetic trader** for every order. The harness gives an `order_id`
  only; PyXchange keys a resting order by `(trader, orderId)`, so the adapter
  uses a single `Trader` for all orders and `(trader, orderId)` collapses to
  `order_id`. PyXchange has no self-match prevention (`handleExecution` never
  inspects trader identity — the engine's own `TradingTest` matches a trader
  against its own resting orders), so one trader for all orders matches across
  them exactly as the anonymous harness book intends.
- **Trades** are surfaced by the engine's single internal per-fill hook
  `OrderBook::notifyExecution`, redirected to the adapter (see "Source patch"):
  it fires once per fill with the aggressor (taker) and resting (maker) order
  ids, the maker's resting price, and the matched quantity. The adapter emits
  one `ME_TRADE` per call (the synthetic-trader scheme makes the engine order
  ids == harness order ids directly; the aggressor's `sequence_number` is
  threaded through a per-call context set just before the submit) and keeps the
  maker's liveness shadow current.
- **Prices**: the workload's signed `int64` ticks are passed straight through
  as the engine's `price_t` (a signed integer price); the `(price, time)`
  comparator preserves tick ordering bit-for-bit. No offset/scale is needed.
- **IOC**: the engine has no IOC flag, so the adapter calls a non-Python
  `newOrderIOC` entry point that runs the match loop (`handleExecution`)
  against the opposite book only and **never inserts** the residual; the
  adapter emits one `CancelAck` for the unfilled remainder, so an IOC order
  never rests.
- **Modify**: no native modify — explicit `cancel` + re-submit at the new
  price/quantity (the reinsert loses queue priority; its crossing fills carry
  the modify message's `sequence_number`, per the harness contract).
- A per-order **liveness shadow** (a flat `std::vector<Live>` indexed by the
  dense harness order id; grow-only, pre-sized to 2^21 entries in
  `engine_init`) holds `{resting, side, price, qty}`. It is required to
  adjudicate cancel / modify of a not-resting order (already filled / never
  rested / never seen → `CancelReject` / `ModifyReject`) and to echo the
  resting order's side/price on the ack (the engine surfaces neither on
  cancel). It is **not** consulted for the audit queries —
  `engine_query_best_bid/ask` and `engine_query_depth_at` read the engine's
  live `OrderContainer` directly (the `idxPrice` index), so a stale shadow can
  never fool the state audit.

`OrderAck` / `CancelAck` / `ModifyAck` / `CancelReject` / `ModifyReject` are
synthesised above the engine, which has no ack/reject callback (its Python
notify path is a no-op in this build). The matcher runs synchronously, so
`engine_flush` is a no-op and no `engine_prebuild` is exported (there is no
translation-only prebuild step; all work happens in the `engine_on_*`
handlers).

## Source patch

`build.sh` patches the engine after `git reset --hard` to the pin (so the reset
can never clobber the patches; every patch is idempotent and fails loud if its
verbatim anchor drifts). The patches fall in two buckets.

### (1) Python-edge decoupling — a build necessity, **not** a behaviour change

The C++ core is welded to the Python runtime through its I/O edges, so to build
and drive it with no interpreter the adapter replaces only those edges with
plain-typed, no-op equivalents. None of these change the matching logic,
prices, or quantities:

- `PyXchange.hpp` drops `#include <boost/python.hpp>` and the boost::python
  `hasattr` helper.
- `Order` gets a **plain-typed constructor** (`side, orderId, price, quantity`)
  in place of the `py::dict` one; the `extract*` `py::dict` statics are dropped.
  The match-relevant accessors (`comparePrice`, `getPrice`, `getTime`, `getId`,
  `getTrader`, `getUnique`) are unchanged.
- `OrderBook` gains non-Python entry points — `newOrder` / `newOrderIOC` /
  `cancel`, plus `bestBid` / `bestAsk` / `depthAt` for the audit queries — that
  **dispatch into the existing private templated workers** (`insertOrder`,
  `handleExecution`, `cancelOrder<>`), so the match logic is reached unchanged.
  The unused `py::dict` public entry points (`createOrder` / `marketOrder` /
  the `py::dict` `cancelOrder`) are trimmed.
- The reporting edge is redirected: `OrderBook::notifyExecution` (the engine's
  native "one call per fill" point) now calls `extern "C" pyx_on_trade(...)`
  implemented by the adapter; `handleExecution` (the match loop) is untouched.
  `Trader` / `Client` / `Logger` are reduced to minimal no-op translation units
  (the Python per-trader/per-client notify and `logging.getLogger()` paths are
  the public market-data tape, not the per-order report stream the harness
  wants). `utils/Exception.hpp` (its inline `raise`/`translate` use
  boost::python) is dropped from the two files that include but never use it.

### (2) Monotonic FIFO time key — the engine correctness fix (`with fix`)

This is the **one behaviour change**, and it is why the harness classifies
pyxchange "with fix":

- `PyXchange.hpp` changes `prio_t` from
  `std::chrono::time_point<std::chrono::high_resolution_clock>` to a plain
  `std::uint64_t`, and `Order.cpp` stamps `Order::time` from a strictly-
  increasing process-wide atomic counter instead of
  `high_resolution_clock::now()`.

  The book's primary match-walk index is an `ordered_unique` on the composite
  `(price, time)` key. With a wall-clock `time`, two same-price orders that land
  on an **equal timestamp** (clock resolution exceeded under a same-tick
  same-price burst) collide on the unique key: the second `insert()` returns
  `.second == false` and the engine **silently drops the order**. A strictly-
  increasing counter — the FIFO time priority the engine already intends — gives
  every order a distinct key, removing the tie/drop hazard deterministically.

  This was **verified to be load-bearing** against this harness: rebuilding the
  engine with the original wall-clock key (at a resolution the same-tick
  same-price burst exceeds) drops orders and the audit reports `Status: FAIL` /
  `Verdict: INVALID` (hash diverges from the `liquibook` baseline); with the
  monotonic counter the same workload is `VALID`. The finding is recorded in
  `CORRECTNESS_FINDINGS.md` ("wall-clock `(price,time)` key drops same-tick
  same-price orders"); it was drafted for upstream but **not filed** (the
  repository has issues disabled), so there is no upstream issue URL to cite.

## Build / run

```bash
bash additional_references/pyxchange_adapter/build.sh
./harness --engine pyxchange_adapter.so --scenario normal --mode audit \
          --matcher-core 68 --drainer-core 69
```

`build.sh` clones the engine into `third_party/pyxchange_PyXchange/` at the
pinned commit, applies the patches above, and compiles the matcher core
(`Order` + the `OrderBook*.cpp` translation units + the non-Python edge units)
together with this adapter into `pyxchange_adapter.so` at the repo root with the
system `g++` (C++17) and the header-only `boost::multi_index` from
`libboost-dev`. Override: `ME_PYXCHANGE_SRC=/path/to/existing/clone` uses an
existing checkout in place of cloning (the `git reset --hard` + the patches are
re-applied idempotently).
