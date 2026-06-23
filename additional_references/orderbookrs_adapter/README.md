# orderbookrs_adapter — integration example

Wraps [joaquinbejar/OrderBook-rs](https://github.com/joaquinbejar/OrderBook-rs)
behind `api/matching_engine_api.h`.

Pinned commit: `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` (orderbook-rs `0.8.0`,
depends on `pricelevel = "0.7"` from crates.io).

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot.

## Engine shape

`OrderBook<T>` is a single-threaded matcher, lock-free internally. Native APIs
visible from the adapter:

- `OrderBook::<()>::with_trade_listener(symbol, listener)` — constructor that
  installs an `Arc<Fn(&TradeResult)>` invoked synchronously from the matching
  loop on every match-producing call.
- `OrderBook::add_limit_order(id, price, qty, side, TimeInForce, extra)` —
  matches the incoming order and rests any residual. Returns `Result<Arc<Order>,
  OrderBookError>`.
- `OrderBook::cancel_order(id)` — returns `Ok(Some(removed_order))` when
  found (carrying the order's side, price, and live remaining quantity) and
  `Ok(None)` when not resting.
- `OrderBook::best_bid()` / `best_ask()` — `Option<u128>`.
- `OrderBook::levels_in_range(min, max, side)` — iterator over `LevelInfo`
  used for exact-price depth queries.

Native order ids are `pricelevel::Id` — a 3-variant enum (`Uuid` / `Ulid` /
`Sequential(u64)`). `Sequential` round-trips a u64 losslessly via `as_u64()`,
so the harness's `uint64_t` order ids carry through trade callbacks without a
sidecar map.

Native order types include Iceberg, Post-Only, FOK, IOC, GTC, GTD, and trailing
stops; the adapter maps harness IOC onto the engine's native
`TimeInForce::Ioc` and GTC onto `TimeInForce::Gtc`.

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports in
the harness's wire format. These are synthesised above the engine.

## Adapter strategy

The upstream is pure Rust, so the adapter is one Rust `cdylib` that exports the
harness `engine_*` extern-C symbols directly. No C++ shim layer in between.
Path A (pure-Rust .so) in the integration menu — strictly less marshaling than
adding a C++ wrapper just to call into Rust.

- A trade listener installed at `OrderBook` construction captures every
  `TradeResult` synchronously inside `add_limit_order`. The adapter reads
  thread-local `(seq, taker_oid)` cells set just before the call to stamp
  each emitted Trade report with the originating message's seq, and the
  listener writes back the taker's post-match `remaining_quantity()` — the
  engine's own answer for what is left of the incoming order.
- **IOC** is submitted as the engine's native `TimeInForce::Ioc`. Trades emit
  through the listener during matching; the engine drops any residual
  internally (an Ioc never rests) and reports an unfilled/partially-filled
  Ioc through its `Err(InsufficientLiquidity { available, .. })` arm. The
  adapter synthesises the harness `CancelAck` for the unfilled remainder
  from those engine-returned quantities — Trades first, then the CancelAck,
  the same emission order as the C++ adapters.
- **Modify** is the harness contract — cancel + reinsert with queue-priority
  lost. The adapter cancels through the engine, emits `ModifyAck` at the new
  price/qty, then re-submits as a new GTC limit so any crossing fills emit
  through the trade listener tagged with the modify's seq.
- **No adapter-side order state.** The engine's id-keyed `cancel_order` is
  both the reject adjudicator and the payload source: `Ok(Some(order))`
  returns the removed order's side, price, and live remaining quantity for
  the CancelAck/ModifyAck echo, and `Ok(None)` (not resting) drives
  CancelReject / ModifyReject. The book and transport handles live in
  single-thread-owned cells (`UnsafeCell` behind a `Sync` shim — the
  `ThreadOwned` idiom shared with the philipgreat and limitbook adapters);
  no lock or atomic sits on the hot path.

## Source patch

`build.sh` applies **one** engine-source patch — `pricelevel-pricetime.patch`,
which touches **two** files in the `pricelevel` crate. This **is** the engine's
"with fix" correctness patch (not adapter instrumentation), so the shipped
engine is the conforming one. It is filed upstream as
[joaquinbejar/OrderBook-rs#88](https://github.com/joaquinbejar/OrderBook-rs/issues/88)
(see `CORRECTNESS_FINDINGS.md`).

The defect is a **price-time-priority violation across partial fills**.
OrderBook-rs does its actual per-price-level matching in the `pricelevel` crate
(`pricelevel = "0.7"`, pinned at `0.7.0`). In `PriceLevel::match_order`, when the
head order at a level is only partially filled, its residual is re-queued with
`self.orders.push(...)`, which lands it at the **tail** of the level's FIFO — so
any same-price order that arrived *after* the original maker is matched ahead of
the maker's remainder on the next trade at that price. Total resting quantity at
the level stays correct, but the `maker_order_id` of subsequent same-price fills
is wrong, which fails the benchmark report-stream hash on all five scenarios
(quantities correct; counterparty wrong).

The patch is an engine-source fix, not an adapter workaround, and lands in two
files:

- **`src/price_level/order_queue.rs`** — gives `OrderQueue` a deque-like backing
  (`Mutex<VecDeque<Id>>` in place of the tail-only `crossbeam` `SegQueue<Id>`),
  updating `new` / `push` / `pop` to match, and adds a new `push_front`
  primitive that returns an order to the **front** of the queue, preserving its
  time priority.
- **`src/price_level/level.rs`** — in `match_order`'s partial-fill arm, re-queues
  the maker's residual via the new `push_front` instead of `push`, so the head
  keeps its original time priority across partial fills. The unrelated tail
  `push` in `update_order` (explicit modify) is left as-is — modify-to-back is
  correct.

`pricelevel`'s own 144 `price_level` unit tests pass unchanged.

Because `pricelevel` comes from crates.io and is a dependency of **both** this
wrapper (direct) and OrderBook-rs (transitive), it cannot be patched in a single
engine clone the way the C++ adapters patch their one checkout. Instead `build.sh`
materialises a pristine `pricelevel-0.7.0` source tree under
`wrapper/pricelevel-patched/` (git-ignored, rebuilt every run), applies the patch
there with `patch -p1 --dry-run` first (an upstream version drift fails loud
rather than silently shipping the bug) and then for real, and hard-verifies the
front-requeue actually landed. The wrapper `Cargo.toml`'s `[patch.crates-io]`
substitutes that patched copy for the crates.io `pricelevel` everywhere, so a
single patched node resolves for both deps and the fix survives any
`git reset --hard` of the engine clone (it lives outside it).

No other engine source is altered. The only other source edit `build.sh` can make
is a build-plumbing path swap: when `ME_ORDERBOOKRS_SRC` overrides the checkout
location it `sed`s the wrapper `Cargo.toml`'s `path = ...` for OrderBook-rs to the
override and restores it on exit — this changes where the engine is read from, not
its logic, and is not a correctness patch.

## Build / run

```bash
bash additional_references/orderbookrs_adapter/build.sh
./harness --engine orderbookrs_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` installs the stable Rust toolchain into `$HOME/.cargo` if `cargo`
is not on PATH (no sudo required), clones the engine into
`third_party/OrderBook-rs/` at the pinned commit, and builds the wrapper
`cdylib` via `cargo --release`. Use `ME_ORDERBOOKRS_SRC=/path/to/checkout`
to skip the clone (the script rewrites the wrapper crate's
`Cargo.toml` path to point at the override and restores it on exit).
