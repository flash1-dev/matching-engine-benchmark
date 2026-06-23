# limitbook_adapter — integration example

Wraps [solarpx/limitbook](https://github.com/solarpx/limitbook) behind
`api/matching_engine_api.h`.

Pinned commit: `943eadc181d1e35a26abaa5217eeb32bf3304267` (`limitbook 0.1.0`,
Apache-2.0; depends on `rust_decimal` (declared `1.37.2`) and `eyre 0.6.x`
from crates.io).

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot.

## Engine shape

`OrderBook` is a single-threaded, `&mut`-driven CLOB: a `BTreeMap<Tick, Orders>`
per side, a `VecDeque<Order>` per price level (FIFO time priority), and a
`HashMap<OrderId, (side, tick)>` for O(1) lookup. Native APIs visible from the
adapter:

- `OrderBook::new(tick_size: Decimal)` — construct an empty book.
- `add_limit_order(side, price, quantity) -> Result<(OrderId, Vec<Fill>)>` —
  synchronous. Matches the incoming order against the book and rests any
  residual; **returns the fills by value**, so the adapter emits `ME_TRADE`
  straight from the return value with no trade callback or thread-local (the
  simplest of the two Rust paths — simpler than `orderbookrs_adapter`, which
  needs a `TradeListener`). `Fill { quantity, price, taker_order_id,
  maker_order_id }` exposes both order ids.
- `cancel_limit_order(id) -> Result<()>` — `Ok` if the order was resting and
  removed, `Err` otherwise.
- `best_bid()` / `best_ask()` — `Option<Decimal>`.
- `execute_market_order(side, qty)` — **not used.** The harness workload is
  limit orders + IOC; the market path errors on insufficient liquidity, so IOC
  is composed as add-limit-then-cancel-residual instead.

Native order types: Limit and Market only — **no modify, no IOC/FOK/GTD**, no
native ack/reject reports in the harness's wire format. Acks, cancel/modify
rejects, the IOC-residual CancelAck, and the modify (= cancel + reinsert) are
all synthesised in the adapter.

### Key adapter detail: the engine assigns its own order ids

`add_limit_order` **ignores any caller-supplied id** and assigns its own
sequential `OrderId` (from `next_order_id()`), which it then uses in the
returned `Fill`s for both maker and taker. The harness, however, carries its
own `order_id` per message and hashes every Trade as
`1,seq,price,qty,maker_order_id,taker_order_id`. The adapter therefore keeps a
bidirectional map (`engine_id <-> harness_id`) and translates every fill's
maker/taker back into harness id space before emitting. This is the main
structural difference from `orderbookrs_adapter`, whose engine round-trips the
caller's id directly.

### Price mapping

The harness speaks **integer ticks** (`price_ticks: i64`); limitbook speaks
`Decimal` on a tick grid. The adapter constructs the book with **tick_size = 1**
and passes `Decimal::from(price_ticks)` straight through, so the engine's price
grid coincides with the harness's integer-tick grid: the round-trip is exact
(no fractional `Decimal`, no rounding), and `best_bid` / `best_ask` /
`depth_at` come back as the same integers the harness compares against its
baseline. (Workload prices are large positive tick counts, ~32k–34k, so the
engine's "price must be positive" guard is never tripped.)

### Other adapter strategy

- **Depth** is maintained as an incremental `HashMap<(price_ticks, side), u64>`
  so `engine_query_depth_at` is O(1): bumped when a residual rests, decremented
  per maker fill and on cancel. (The engine's own per-level volume cache is
  `pub(crate)` and not reachable from an external wrapper crate, and a separate
  per-level accessor would have meant patching upstream — the incremental index
  avoids that.)
- **IOC** is added as a plain limit; the engine rests any residual, which the
  adapter then cancels, emitting one `CancelAck` for the unfilled remainder
  (Trades first, then the residual CancelAck — order-independent anyway, since
  the harness sorts the stream by `(seq, type)`).
- **Modify** is the harness contract — cancel + reinsert, queue priority lost:
  cancel through the engine, emit one `ModifyAck` at the new price/qty, re-add
  as a new limit so crossing fills emit tagged with the modify's seq.
- A shadow `harness_id -> {engine_id, price, side, remaining}` drives the
  cancel/modify reject paths and the CancelAck/ModifyAck side+price echo,
  mirroring the other reference adapters. Presence in the map is the liveness
  flag: entries are removed on every terminal transition.
- The whole adapter (engine + both id maps + depth index) lives in one
  single-thread-owned `UnsafeCell` cell (the `ThreadOwned` idiom shared with
  the philipgreat adapter): the matcher thread is the only accessor, so no
  lock and no atomic appears on the hot path. The cell exists because the
  engine's `OrderBook` mutates through `&mut self` only, and a `static` needs
  interior mutability.

## Source patch

`build.sh` applies the engine's **with-fix correctness patch** (this is the
"with fix" measurement), after `git reset --hard` to the pin so the reset can
never clobber it (idempotent; fails loud if its anchors drift). Two source
edits to the engine, plus one build-config edit gated on an override:

- **Partial-fill write-back** (correctness fix, `src/order_book.rs`, **three
  sites**). As shipped, a resting order that takes a *partial* fill is never
  shrunk: `resting_order` is a `front_mut()` `&mut Order` whose `quantity` is
  read to compute the fill and used to decrement the taker's remaining, the
  level's `total_volume` and the book counter, but **never written back to the
  maker itself**. The removal test `if fill_quantity == resting_order.quantity`
  therefore stays false on a partial fill, so the maker sits at the queue front
  at its *original* size and is matchable again and again — the book hands out
  more shares than ever rested (~4.3× over-match on `normal`; full symptom table
  below). The pristine `pop_front` removal block appears identically at all
  three matching sites — the buy-side and sell-side loops in `add_limit_order`
  (nested one level deeper) and the loop in `execute_market_order` — so the
  patch adds an `else` branch (`resting_order.quantity -= fill_quantity;`) at
  each, popping only on full consumption and decrementing the maker's own
  quantity on a partial fill. This is the engine's real defect, filed upstream
  as [solarpx/limitbook#1](https://github.com/solarpx/limitbook/issues/1); with
  it the report stream is byte-identical to the consensus and limitbook is
  **VALID ×5** (see the verdict below). `resting_order` is already `&mut Order`
  and `Order::quantity` is a public `Decimal`, so each site is the one added
  `else` branch and nothing else in the matching logic changes.
- **Doc-comment relaxed** (cosmetic follow-on, `src/order.rs`). The fix above
  mutates `Order::quantity` as a resting order fills, so the engine's
  "All fields are immutable after creation" doc line is no longer true; the
  patch rewrites it to say `id`/`order_type`/`order_side` are fixed at creation
  while `quantity` is reduced as the order fills. A doc comment only — no
  behaviour change — but kept accurate because the correctness patch makes the
  original statement false.
- **Cargo.toml dep path** (build-config, `wrapper/Cargo.toml`, **conditional**).
  The wrapper crate depends on `../../../third_party/limitbook` by relative
  path. *Only when* `ME_LIMITBOOK_SRC` overrides the default checkout does
  `build.sh` `sed` that path to the override and restore it on exit (a `trap`);
  in the default build this edit does not run. Pure build-config, no engine
  semantics — listed for completeness so this section matches `build.sh` exactly.

The first two edits are idempotent: the `git reset --hard` restores pristine
sources each rerun, the `order_book.rs` patch is guarded by a `PATCH(...)`
marker (re-apply is a no-op) and fails loud if its three anchor blocks are not
found, and the `order.rs` edit is an exact-string no-op on rerun. Unlike the
`kautenja_adapter` patch (pure adapter instrumentation, *not* that engine's
correctness fix), the `order_book.rs` patch here **is** the upstream-filed
correctness fix — see the verdict below for the bug it resolves.

## Build / run

```bash
bash additional_references/limitbook_adapter/build.sh
./harness --engine ./limitbook_adapter.so --scenario normal --mode perf  \
          --matcher-core 84 --drainer-core 85
./harness --engine ./limitbook_adapter.so --scenario normal --mode audit \
          --matcher-core 84 --drainer-core 85
```

`build.sh` installs the stable Rust toolchain into `$HOME/.cargo` if `cargo`
is not on PATH (no sudo), clones the engine into `third_party/limitbook/` and
hard-resets it to the pinned commit (idempotent), and builds the wrapper
`cdylib` with `cargo build --release` + `RUSTFLAGS="-C target-cpu=native"`
(the house build recipe — no extra optimization tuning beyond `target-cpu=native`; limitbook's own
`Cargo.toml` declares no `[profile.release]`, so the wrapper adds none). It
copies the result to `limitbook_adapter.so` at the repository root. Override
the checkout with `ME_LIMITBOOK_SRC=/path/to/checkout`.

## Advertised performance vs. measured

limitbook's README advertises:

- **Limit orders: 3–5 million orders/second** (~204 ns non-crossing, ~290 ns
  crossing).
- **Market orders & cancellations: ~31 ns each (~30 million/second).**

Those are single-op in-process Criterion microbenchmarks (no inter-thread
report emission, no full lifecycle). Under the harness's `normal` scenario
(1M orders + cancels/modifies/IOC, every report emitted across an inter-thread
SPSC drainer), the limit-order path measured **2.72 M msgs/s** (median of 10
trials, dedicated cores 82/83) — i.e. at the low end of, and consistent
with, the advertised 3–5 M/s limit-order ceiling once the full
report-emission cost is paid.

## Correctness: INVALID as shipped → VALID ×5 with the adapter's filed fix (upstream matching bug)

As shipped, the pinned engine is **INVALID** — both `perf` (report-stream hash)
and `audit` (state audit, 155/192 probes mismatched the `liquibook` baseline on
`normal`) fail. The cause is a genuine correctness bug **in limitbook itself**, not
in the adapter; `build.sh` applies the upstream-filed fix (the partial-fill
write-back `else` branch at each of the three matching sites, plus a follow-on
doc-comment edit — see the **Source patch** section), after which limitbook is
byte-identical to the consensus and VALID ×5. The bug, as shipped:

**Partial fills never decrement the resting (maker) order's quantity.** In
`src/order_book.rs`, the inner matching loop of `add_limit_order` (and the
identical loop in `execute_market_order`) takes `let resting_order =
orders.orders.front_mut()`, computes `fill_quantity =
remaining.min(resting_order.quantity)`, pushes the `Fill`, decrements
`remaining_quantity` and the **level's** `total_volume` cache — but **never
writes `resting_order.quantity -= fill_quantity`**. The resting order is only
removed when the fill exactly equals its *original* quantity
(`if fill_quantity == resting_order.quantity { pop_front }`). So a maker that is
partially filled keeps its full original size and can be re-filled, up to that
full size, by every subsequent aggressor.

Minimal reproduction against the pinned engine (no adapter involved):

```text
rest BUY 100 @ p;  SELL 30 @ p -> fill 30;  SELL 30 @ p -> fill 30;
SELL 100 @ p -> fills 100   (a correct book has only 40 left → fills 40)
```

First harness divergence (`normal`, canonical seed): resting sell `oid
889598` (53 shares @ 33881, placed at seq 124) is partially filled for 14
shares at seq 125 — 39 left. At `seq 147` the consensus fills those
remaining 39; limitbook — which never decremented the maker — prints **53**.

How it presents across the whole `normal` stream — every symptom follows from
phantom maker liquidity that is never drained:

| report type   | limitbook | canonical | ratio |
|---------------|-----------|-----------|-------|
| OrderAck      | 1,000,000 | 1,000,000 | 1.00× |
| Trade         |   268,154 |    62,474 | 4.29× (over-matching) |
| CancelAck     |   706,100 |   931,363 | 0.76× |
| ModifyAck     |   125,115 |   164,857 | 0.76× |
| CancelReject  |   228,157 |    38,131 | 5.98× (orders filled away early) |
| ModifyReject  |    47,560 |     7,818 | 6.08× |

Strict price-time priority with correct partial-fill accounting is the
invariant the harness's baseline holds; limitbook at the pinned commit, unpatched,
does not, so the report-stream hash and the state audit both fail. Run unpatched,
the first 48 messages (which contain no resting partial fill) hash
byte-identically to canonical, which confirms the adapter's ABI mapping, id
translation, IOC synthesis, modify path, and report formatting are correct and
that the divergence originates entirely in the engine. The adapter's `build.sh`
then applies the partial-fill write-back fix (the added `else` branch at all
three matching sites — see the **Source patch** section) — filed upstream as
[solarpx/limitbook#1](https://github.com/solarpx/limitbook/issues/1) — after which
limitbook is byte-identical to the consensus and **VALID ×5**, its
conforming-"with fix" status in `CONSENSUS_CONFORMING_ENGINES.md`.
