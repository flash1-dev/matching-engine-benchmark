# orderbookrs_adapter — integration example

Wraps [joaquinbejar/OrderBook-rs](https://github.com/joaquinbejar/OrderBook-rs)
behind `api/matching_engine_api.h`.

Pinned commit: `53b4d2b0a657f4260e316d3a8ac3f0df0fc068bf` (orderbook-rs `0.8.0`,
depends on `pricelevel = "0.7"` from crates.io).

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `discoveries.md` at the repository
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
- `OrderBook::cancel_order(id)` — returns `Ok(Some(_))` when found,
  `Ok(None)` when not.
- `OrderBook::best_bid()` / `best_ask()` — `Option<u128>`.
- `OrderBook::levels_in_range(min, max, side)` — iterator over `LevelInfo`
  used for exact-price depth queries.
- `OrderBook::get_order(id)` — `Option<Arc<OrderType>>` for reading the resting
  quantity back after a match.

Native order ids are `pricelevel::Id` — a 3-variant enum (`Uuid` / `Ulid` /
`Sequential(u64)`). `Sequential` round-trips a u64 losslessly via `as_u64()`,
so the harness's `uint64_t` order ids carry through trade callbacks without a
sidecar map.

Native order types include Iceberg, Post-Only, FOK, IOC, GTC, GTD, and trailing
stops; this adapter uses GTC only and synthesises the harness's IOC residual
behavior in the adapter (so report ordering is deterministic regardless of how
the engine decomposed any fills).

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports in
the harness's wire format. These are synthesised above the engine.

## Adapter strategy

The upstream is pure Rust, so the adapter is one Rust `cdylib` that exports the
harness `engine_*` extern-C symbols directly. No C++ shim layer in between.
Path A (pure-Rust .so) in the integration menu — strictly less marshaling than
adding a C++ wrapper just to call into Rust.

- A trade listener installed at `OrderBook` construction captures every
  `TradeResult` synchronously inside `add_limit_order`. The adapter reads a
  thread-local `(seq, taker_oid)` set just before the call to stamp each emitted
  Trade report with the originating message's seq.
- **IOC** is submitted as GTC; if the engine leaves a resting residual the
  adapter calls `cancel_order` on the new id and emits a `CancelAck` for the
  unfilled quantity. Doing it this way instead of `TimeInForce::Ioc` keeps the
  emission order deterministic — Trades from the listener first, then the
  CancelAck — which matches how the C++ adapters order the same reports.
- **Modify** is the harness contract — cancel + reinsert with queue-priority
  lost. The adapter cancels through the engine, emits `ModifyAck` at the new
  price/qty, then re-submits as a new GTC limit so any crossing fills emit
  through the trade listener tagged with the modify's seq.
- A shadow map `oid -> {side, price, remaining, alive}` drives the reject path
  and CancelAck/ModifyAck side/price echo, matching the C++ adapters' approach.
  Maintained from the same listener that emits trades — partial fills decrement,
  full fills mark dead.

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
