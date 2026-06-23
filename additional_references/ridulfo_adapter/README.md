# ridulfo_adapter — integration example

Wraps [ridulfo/order-matching-engine](https://github.com/ridulfo/order-matching-engine)
("ridulfo") behind `api/matching_engine_api.h`. ridulfo is a pure-Python matching
engine (price-time priority, an `Orderbook` over two
`sortedcontainers.SortedList` instances), so there is no native engine library to
link: the adapter embeds CPython and drives the engine's `Orderbook` class
through the C-API.

Pinned commit:
- `ridulfo/order-matching-engine` — `30fdbf579671325cf682492037d804b03b5baceb`

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observation the harness produced against this snapshot
(ridulfo: with fix — `__lt__` total order; see **Source patch** below).

## Engine shape

`Orderbook` facade, single-threaded synchronous matcher. Native APIs the adapter
uses, all on the engine's own classes:

- `Orderbook.process_order(order)` — submits a `LimitOrder` / `MarketOrder` /
  `CancelOrder`; matches it against the opposite side and appends one `Trade` per
  fill to `Orderbook.trades` (`Trade.price` = MAKER resting price,
  `Trade.book_order_id` = maker id), in match order; any limit residual rests in
  `Orderbook.bids` / `.asks`.
- `Orderbook.bids` / `Orderbook.asks` — the two `SortedList`s of resting orders,
  read directly as the authoritative "is this order resting?" oracle (the engine
  reports no result code) and to echo a resting order's side/price on a
  CancelAck, and to discard an order (`SortedList.discard`).
- `Orderbook.get_bid()` / `get_ask()` — best bid/ask price for the state-audit
  queries; depth is summed from the resting orders at a price on the chosen side.

Not provided natively: no IOC / FOK / POST-ONLY flag; no price-changing modify
type (`CancelOrder` is the only mutation); `CancelOrder` is a silent no-op
whether or not the order was resting (no success/reject signal); the engine emits
no explicit Ack / Reject events (the adapter synthesises those from native
state). `Order.order_id` is the harness's own id, so no id→handle map is built.

## Adapter strategy

The C++ side (`ridulfo_adapter.cpp`) embeds CPython once in `engine_init`,
imports a thin orchestration module (`ridulfo_helper.py`), and bridges each
`engine_*` ABI call to a helper function. The helper calls the engine's native
`Orderbook` API and **never reimplements matching**; it returns, per message, the
engine's own trade tuples plus the small amount of liveness state the engine API
cannot express, which the C++ side turns into the six `me_report_t` kinds and
pushes into the harness report transport.

- **Reports come from the engine.** A Trade is emitted for each row ridulfo
  appended to `Orderbook.trades`; `Trade.price_ticks` is the maker's resting
  price (`Trade.price`, which the engine fills from the resting `bookOrder.price`)
  and `Trade.sequence_number` is the aggressor's.
- **Prices**: the harness uses signed integer ticks; ridulfo keys its book by the
  same integer `price`, passed straight through — no float round-trip.
- **Order-id mapping**: none needed. ridulfo names orders with the harness's own
  id (`Order.order_id`), so the harness id is passed straight through. No
  adapter-side id→handle map is built.
- **Arrival-time FIFO**: `Order.time` is normally `int(1e6*time())` (wall-clock
  microseconds). The helper instead stamps each order with a strictly increasing
  arrival counter, so the run is deterministic and the engine gets true
  first-in-first-out tie-breaking among equal-price orders (its intended
  arrival-time semantics). This is the engine's own `Order.time` field — not a
  shadow ordering.
- **IOC**: submit as a normal native `LimitOrder` (so it crosses exactly where
  the engine's own price logic lets it), then, if a residual rested, `discard`
  that exact residual object from the engine's book and emit one CancelAck (the
  engine has no IOC flag). No matching is reimplemented — only the "do not rest
  the remainder" rule is applied.
- **Modify**: native cancel (`CancelOrder`) + reinsert at the new price/qty
  (loses time priority; the reinsert may itself cross and print Trades), then one
  ModifyAck; ModifyReject if the order is not resting. ridulfo has no modify type.
- **Cancel / modify of a non-resting order** (already filled, already cancelled,
  or never seen) → CancelReject / ModifyReject, adjudicated by scanning the
  engine's own `Orderbook.bids` / `.asks` for the id — the engine's true state,
  not an adapter shadow book.

`engine_flush` is a no-op: matching and report emission complete synchronously
inside each `engine_on_*` call, so the pipeline is already drained. The adapter
exports no `engine_prebuild`, so the harness runs no prebuild pass (nothing is
inserted into the book ahead of the timed run). The harness reports two engine
threads "after_init"; these are the embedded interpreter's own runtime threads,
not matcher workers — all matching runs on the harness's matcher thread, which
boots the interpreter and holds the GIL through every `engine_*` call via a
single cached thread state.

## Source patch

**One correctness patch**, applied by `build.sh` to the engine clone
(`ordermatchinengine/Order.py`):

`LimitOrder.__lt__` is corrected to a **consistent total order** — a stable final
tiebreak on the unique `order_id`. The upstream comparator
(`Order.py:43`) ordered by price, then `time`, then **size**, and returned
`None` when price, time and size were all equal:

```python
        elif self.time != other.time:
             return self.time < other.time
        elif self.size != other.size:        # priority inversion on equal price+time
            return self.size < other.size     # (falls through to None on a full tie)
```

Two failures follow on a `time` tie (which the engine's microsecond stamping
produces routinely at ~400k orders/s): the size compare lets a smaller order jump
ahead of an older equal-price order (a price-time-priority violation), and on a
full price/time/size tie `__lt__` returns `None`, so neither `a < b` nor `b < a`
holds — `SortedList` then treats the two as equal and `SortedList.discard()` can
no longer locate a cancelled order, leaking it. The patch drops the size compare
and ends on `return self.order_id < other.order_id`, which is a strict total
order, so cancels always resolve and equal-price priority is well-defined.
Reported upstream: [ridulfo/order-matching-engine#10](https://github.com/ridulfo/order-matching-engine/issues/10).

The patch is applied post-`reset`, idempotent (a marker guard makes a re-apply
under `ME_RIDULFO_SRC` a no-op), and anchor-checked (the build aborts loudly if
the upstream `__lt__` body has moved, rather than silently shipping unpatched).

Why the canonical `normal` run is VALID either way, and why the fix still ships:
the adapter's arrival counter already keeps `Order.time` unique per order, so the
size/`None` fall-through is never reached on the canonical tape — `git reset
--hard` to the pristine engine (no patch) also passes `normal`. The fix is the
**documented engine correctness fix** and is load-bearing the moment two
equal-price orders share a `time`: feeding the unpatched engine a tape with
genuine time ties reproduces both failures (the state audit mismatches the
baseline and extra/incorrect trades print); the patched engine clears the state
audit, and the patched engine plus the arrival-counter FIFO reproduces the
canonical hash exactly. Per the project's adapter mandate, a broken engine API is
repaired with a minimal documented engine patch rather than masked in the
adapter, so the engine ships patched.

## Build / run

```bash
bash additional_references/ridulfo_adapter/build.sh
./harness --engine ridulfo_adapter.so --scenario normal --mode audit \
          --matcher-core 70 --drainer-core 71
```

`build.sh` clones the engine into `third_party/ridulfo_order_matching_engine/` at
the pinned commit, applies the `__lt__` patch above, vendors
`sortedcontainers==2.4.0` (the engine's `requirements.txt` pin) into
`third_party/ridulfo_vendor/`, then compiles `ridulfo_adapter.cpp` against the
embeddable system CPython (`python3-config --embed`) into `ridulfo_adapter.so` at
the repo root. The engine clone dir, this adapter dir, and the vendor dir are
baked into the `.so`'s `sys.path` (`-DRIDULFO_REPO_DIR` / `-DRIDULFO_ADAPTER_DIR`
/ `-DRIDULFO_VENDOR_DIR`) so it loads regardless of the harness's working
directory. (`third_party/` is gitignored — the clone and the vendored dep are
build products, not committed sources.)

Requirements: an embeddable system `python3` (the `python3-dev` headers, for
`python3-config --embed`); the embed headers cannot be auto-installed without
root and must already be present. `sortedcontainers` is vendored by `build.sh`
from a `pip3 download` source archive (no system/site install needed). Override
the upstream checkout with `ME_RIDULFO_SRC=/path/to/existing/clone` (the dir must
contain `ordermatchinengine/`).
