# dyn4mik3_adapter — integration example

Wraps [dyn4mik3/OrderBook](https://github.com/dyn4mik3/OrderBook) behind
`api/matching_engine_api.h`. dyn4mik3/OrderBook is a pure-Python price-time
matching engine (a red-black `OrderTree` over `sortedcontainers.SortedDict`
per side, FIFO `OrderList` doubly-linked lists per price level), so there is no
native engine library to link: the adapter embeds CPython and drives the
engine's `OrderBook` class through the C-API.

Pinned commit:
- `dyn4mik3/OrderBook` — `a802407d12d2a21d0c8d65d44cc93dc5634f576b`

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot
(dyn4mik3: a 1-line fix is required, documented under **Source patch** below).

## Engine shape

`OrderBook` facade, single-threaded synchronous matcher. Native APIs the adapter
uses (all are the engine's own — no matching is reimplemented adapter-side):

- `OrderBook.process_order(quote, from_data, verbose)` — submits a limit (or
  market) order; crosses it against the opposite side and returns
  `(trades, order_in_book)`. Each trade record carries `price` (the MAKER
  resting price), `quantity`, and `party1` `[counter_trade_id, side,
  maker_order_id, new_book_qty]`; `order_in_book` is the residual quote dict
  when the incoming order did not fully fill. With `from_data=True` the engine
  honours the caller's `order_id` and `timestamp` verbatim and does **not** use
  its own auto-increment id.
- `OrderBook.cancel_order(side, order_id)` — native cancel-by-id of a resting
  order.
- `OrderTree.order_exists(id)` / `get_order(id)` — the engine's per-side
  `order_map` is the authoritative "is this order resting?" oracle (a
  fully-filled or cancelled maker has been removed from it), and yields the
  resting `Order` (for its side/price) on a CancelAck.
- `OrderBook.get_best_bid()` / `get_best_ask()` / `get_volume_at_price(side,
  price)` — book snapshots for the harness state-audit queries.

Not provided natively: no IOC / FOK / POST-ONLY flag; the engine's in-place
`update_order` does **not** cross the book on a reprice (so it cannot express a
price-changing modify on its own); the engine emits no explicit Ack / Reject
events (the adapter synthesises those from native state).

## Adapter strategy

The C++ side (`dyn4mik3_adapter.cpp`) embeds CPython once in `engine_init`,
imports a thin orchestration module (`dyn4mik3_driver.py`), and bridges each
`engine_*` ABI call to a driver method. The driver calls the engine's native
`OrderBook` API and **never reimplements matching**; it returns, per message, a
list of report tuples (first element = the `me_report_type_t` value) which the
C++ side unpacks and pushes into the harness report transport.

- **Reports come from the engine.** A Trade is emitted for each record the
  engine appended to its returned `trades` list; `Trade.price_ticks` is the
  maker's resting price (the engine stores the maker price on the trade) and
  the maker id is `party1[2]`. `Trade.sequence_number` is the aggressor's.
- **Prices**: the harness uses signed integer ticks; the engine keys its book
  by `Decimal`. The driver passes the integer tick straight through as the
  quote `price` (the engine wraps it in `Decimal`, exact) and converts the
  maker price back with `int(...)`. Tick ordering is preserved exactly.
- **Order-id mapping**: none needed — `from_data=True` makes the engine name
  orders with the harness's own `order_id` (and label time with the harness
  `sequence_number`), so the harness id is passed straight through. No
  adapter-side id→handle map is built.
- **IOC**: submit as a normal limit, let it cross, then `cancel_order` the
  rested residual (`order_in_book`) and emit one CancelAck (the engine has no
  IOC flag).
- **Modify**: native `cancel_order` + reinsert via `process_order` at the new
  price/qty (both engine calls), so the reinsert itself crosses the book and
  loses time priority — the harness modify contract; one ModifyAck follows
  (and any crossing fills are emitted as Trades). The engine's in-place
  `update_order` is not used because it does not cross on a reprice.
  ModifyReject if the order is not resting.
- **Cancel / modify of a non-resting order** (already filled, already
  cancelled, or never seen) → CancelReject / ModifyReject, adjudicated via the
  engine's own `OrderTree.order_exists`, i.e. the engine's per-side `order_map`
  — no adapter shadow book.

`engine_flush` is a no-op: matching and report emission complete synchronously
inside each `engine_on_*` call, so the pipeline is already drained. The adapter
exports **no** `engine_prebuild`, so the harness runs no prebuild pass (nothing
is inserted into the book ahead of the timed run). The harness may report engine
threads "after_init"; these are the embedded interpreter's own runtime threads,
not matcher workers — all matching runs on the harness's matcher thread under
the GIL.

## Source patch

One line, applied by `build.sh` after the `git reset --hard` (so the default
checkout is pristine each run), idempotent, and anchor-checked so an upstream
change cannot silently leave the bug in place.

`OrderBook.get_volume_at_price` (`orderbook/orderbook.py`) calls
`self.{bids,asks}.get_price(price).volume`, but `OrderTree` defines **no**
`get_price` method — only `get_price_list` (`orderbook/ordertree.py`). The
engine's public depth-at-a-price query therefore raises `AttributeError` and
**crashes** whenever the queried level exists, and the harness's
`engine_query_depth_at` (driven from the state audit) exercises exactly that.
The fix is the obvious one: `get_price` → `get_price_list` (the method that
returns the price level's `OrderList`, which carries `.volume`). With the bug
present the engine is INVALID (it crashes on the audit's depth query); with the
one-token fix it is VALID. Quantity, matching, and book state are untouched.

Reported upstream: <https://github.com/dyn4mik3/OrderBook/issues/22>.
`build.sh` replaces both occurrences (the `bid` and `ask` branches) and then
asserts the fixed form is present and the buggy form is gone, failing the build
loudly otherwise.

## Build / run

```bash
bash additional_references/dyn4mik3_adapter/build.sh
./harness --engine dyn4mik3_adapter.so --scenario normal --mode audit \
          --matcher-core 64 --drainer-core 65
```

`build.sh` clones the engine into `third_party/dyn4mik3_orderbook/` at the
pinned commit, `git reset --hard`s it, applies the one-line depth-query patch,
then compiles `dyn4mik3_adapter.cpp` against the embeddable system CPython
(`python3-config --embed`) into `dyn4mik3_adapter.so` at the repo root. Three
absolute dirs are baked into the `.so`'s `sys.path` so it loads regardless of
the harness's working directory: the engine clone (`-DME_REPO_DIR`, for the
`orderbook/` package), this adapter dir (`-DME_ADAPTER_DIR`, for
`dyn4mik3_driver.py`), and the vendored `sortedcontainers` tree
(`-DME_VENDOR_DIR`).

Requirements: an embeddable system `python3` (the `python3-dev` headers, for
`python3-config --embed`). The engine's only third-party dependency,
`sortedcontainers` (pure Python), is vendored from its sdist into
`third_party/dyn4mik3_vendor/` so the build is hermetic and does not touch the
system Python; the embed headers cannot be auto-installed without root and must
already be present. Override the upstream checkout with
`ME_DYN4MIK3_SRC=/path/to/existing/clone` (the dir must contain the
`orderbook/` engine package).
