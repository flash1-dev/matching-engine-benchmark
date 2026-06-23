# jeog_adapter — integration example

Wraps [jeog/SimpleOrderbook](https://github.com/jeog/SimpleOrderbook)
(Jonathon Ogden) behind `api/matching_engine_api.h`. The engine is a
price-time-priority limit-order book whose price levels are a flat,
directly-indexed `std::vector<level>` spanning a fixed `[min, max]` tick range
(one slot per tick).

Pinned commit:
- `jeog/SimpleOrderbook` — `3411cebb9756b80fd2cb3b442cfb109ca853068b`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`sob::SimpleOrderbook` (`include/simpleorderbook.hpp`), reached through the
`FullInterface` / `ManagementInterface` abstract bases. Orders enter through an
internal order queue serviced by a dispatcher thread, but the public
*synchronous* entry points block on a `std::future` until the dispatcher has
fully processed the order under the engine's master mutex, then run that order's
synchronous execution callbacks on the **calling** thread before returning — so
from the adapter's view every call is synchronous and ordered. Native APIs
visible from the adapter:

- `SimpleOrderbook::BuildFactoryProxy<TickRatio>().create(min, max)` —
  constructs a book over a fixed tick range; `TickRatio = std::ratio<1,1>`
  gives tick size 1.0, so engine price (a `double`) equals the harness integer
  tick directly.
- `insert_limit_order(is_buy, price, size, exec_cb)` — submits a limit order,
  crosses what it can against the opposite side, rests the residual, and returns
  the engine's `id_type` for the order. Ids are dense (`++_last_id` per limit
  insert); `last_id()` exposes the most recent.
- `pull_order(id)` — cancels a resting order; returns `bool` (true iff the order
  was resting) — the engine's own native liveness answer.
- the per-fill execution callback `exec_cb(msg, id1, id2, price, size)` fires
  synchronously on the calling thread, **once per side** per fill, each as
  `(callback_msg::fill, own_id, own_id, maker_price, qty)`.
- `bid_price()` / `ask_price()` — best price per side (returns `0.0` when that
  side is empty); `bid_depth(depth)` / `ask_depth(depth)` — price → aggregate
  resting qty maps; `is_valid_price(p)` — range check.
- `ManagementInterface::grow_book_below/above(price)` — extend the fixed tick
  range (untimed, rare; used only if a held-out price falls outside the book).

Not provided natively: no partial-IOC for limit orders (the engine's advanced
FOK is all-or-none, different semantics); no native modify that preserves the
harness's cancel+reinsert contract; the engine names orders with its own ids
(harness uses `uint64_t`); no ack/reject events.

## Adapter strategy

- **Trades** come from the engine's per-fill execution callback. Because it
  fires once per side, the adapter emits exactly one `ME_TRADE` on the **maker**
  side (the side whose id is *not* the order currently being inserted): the
  taker is the active insert, whose engine id is captured in a per-call context
  just before `insert_limit_order`, so of each callback pair the other id is the
  maker. The Trade carries the maker's resting price (which is what the engine
  passes), the fill quantity, and the maker/taker harness ids — fills come out
  in match order with no double counting.
- **Prices**: the book uses `std::ratio<1,1>` (tick size 1.0), so the engine
  price equals the harness integer tick directly. The book is created spanning a
  wide positive tick range `[1, 2^18-1]` that covers every canonical/held-out
  workload (mid ~33504 ticks, widest observed swing ~`[8900, 81700]`); an
  out-of-range price grows the book via `ManagementInterface` (untimed, rare).
- **IOC**: the engine has no native partial-IOC for limit orders, so the order
  is submitted as a plain limit, matches what it can, and the adapter then pulls
  any rested residual (via the engine's native `pull_order`) and emits one
  `CancelAck` for it — identical to a native match-what-you-can-drop-the-rest
  IOC, so an IOC order never rests.
- **Modify**: cancel + reinsert (the harness contract) — `pull_order` the old
  order, then `insert_limit_order` a fresh limit at the new price/quantity (new
  engine id, losing queue priority) and remap the harness id to it. The engine's
  own `replace_with_limit_order` does the same pull+reinsert internally, but
  doing it in the adapter lets the `pull_order` return code adjudicate
  "not resting" → `ModifyReject` and control the id remap.
- **Cancel/Modify reject**: `pull_order(id)` returns `false` for an order that
  is not resting (never seen / already filled / already pulled) — the engine's
  own native liveness answer — which becomes `CancelReject` / `ModifyReject`.
- Two flat **translation vectors** indexed by the dense ids — harness id →
  engine id (for cancel/modify) and engine id → harness id (to label fills) —
  replace hash maps because both id streams are dense. A minimal identity shadow
  (`side`/`price` per harness id) is recorded at each (re)insert so the
  `CancelAck` can echo the resting order's side and price (the harness
  `cancel_t` carries neither, and the canonical form hashes both). The shadow is
  **not** consulted for the audit queries — `engine_query_best_bid/ask` and
  `engine_query_depth_at` read the engine's live book directly
  (`bid_price`/`ask_price`/`bid_depth`/`ask_depth`), so a stale shadow can never
  fool the state audit.

`OrderAck` / `CancelAck` / `ModifyAck` / `CancelReject` / `ModifyReject` are
synthesised above the engine, which has no ack/reject callback. The synchronous
API has fully processed and reported every delivered order by the time the call
returns, so `engine_flush` is a no-op. `engine_prebuild` is exported but is
**translation-only**: it pre-sizes the harness→engine vector capacity for an
incoming id (and nothing else — no order is built, inserted, matched, or
id-registered), so the harness's post-prebuild "book is empty" assertion holds
and every per-order write stays on the clock.

## Source patch

**No source patch.** The engine is compiled exactly as shipped at the pinned
commit; the harness audit is `VALID` against the unmodified library. jeog is
classified "as shipped" in `CORRECTNESS_FINDINGS.md` and
`CONSENSUS_CONFORMING_ENGINES.md` (no upstream issue filed). `build.sh` does a
`git reset --hard` to the pin and compiles the seven library translation units
without touching any of them.

## Build / run

```bash
bash additional_references/jeog_adapter/build.sh
./harness --engine jeog_adapter.so --scenario normal --mode audit \
          --matcher-core 56 --drainer-core 57
```

`build.sh` clones the engine into `third_party/jeog_simpleorderbook/` at the
pinned commit (the engine has no submodules), then compiles its seven library
`.cpp` files (`advanced_order.cpp`, `simpleorderbook.cpp`, and
`orderbook/{core,orders,objects,query,advanced}.cpp`) plus this adapter into
`jeog_adapter.so` at the repo root with the system `g++` (`-std=c++14`, newer
than the engine's `-std=c++11` floor), linking `-lpthread`. Override:
`ME_JEOG_SRC=/path/to/existing/clone` uses an existing checkout in place of
cloning.
