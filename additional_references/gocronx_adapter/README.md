# gocronx_adapter — integration example

Wraps [gocronx/matcher](https://github.com/gocronx/matcher) behind
`api/matching_engine_api.h`. The engine is pure Rust, so the entire adapter is
one Rust `cdylib` that exports the harness `engine_*` extern-C symbols and calls
straight into the engine crate's order book. No C++ shim.

Pinned commit: `b8d48356c8a2677e0d8a1965d754e3c4884bb947`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot
(gocronx is consensus-conforming **as shipped** — no correctness fix required).

## Engine shape

`matcher::OrderBook` is a single-symbol, single-threaded, lock-free, I/O-free
price-time book: a `BTreeMap<Price, PriceLevel>` per side, an
`AHashMap<OrderId, Order>` for id lookup, and a `SmallVec<[OrderId; 8]>` FIFO per
level. It matches synchronously on the calling thread and returns the *full*
execution event stream from a single call — exactly what the harness needs.
Native APIs visible from the adapter:

- `OrderBook::new()` — constructs the single book.
- `OrderBook::submit_events(order, ts) -> Vec<BookEvent>` — matches the incoming
  order against the book, rests a `Limit` residual (or, for an `Ioc`, drops the
  residual itself with no event), and returns the ordered event stream:
  `Accepted`, `Trade`(s), `Rested`, or `Rejected`. Each `Trade` carries the
  aggressor side, the two ids (`buy_id` / `sell_id`), and the maker's resting
  price — exactly the harness Trade fields.
- `OrderBook::cancel_events(id, ts) -> Vec<BookEvent>` — id-keyed; returns
  `Canceled` (the native cancel-ack) when the order was resting, or
  `CancelRejected{UnknownOrderId}` when it was not (already filled / cancelled /
  never seen).
- `Order::limit(id, side, price, qty)` / `Order::ioc(...)` — the order
  constructors (GTC limit vs immediate-or-cancel).
- `OrderBook::best_bid()` / `best_ask()` — `Option<Price>` (u64 ticks).
- `OrderBook::level_qty(side, price) -> Quantity` — exact resting depth at one
  price level; answers `engine_query_depth_at` directly.
- `OrderBook::get_order_se(id) -> Option<(Side, Price)>` — a minimal **read-only
  accessor added by `build.sh`** (see *Source patch* below): the engine ships no
  public id→order getter, and the CancelAck wire line needs the resting order's
  side+price, which the `Canceled` event omits.

The engine keys every resting order by the caller's id in its own `orders` map,
so that map is the adapter's liveness/reject oracle — **the adapter keeps no
per-order shadow state**. Rejects come from the engine's own `CancelRejected`,
and the CancelAck side/price are read from the engine's own `orders` (via the
read-only accessor), so a resting / side / price disagreement would surface as
an *engine* bug rather than being masked (or invented) by an adapter-side copy.

Native order types: `Limit` and `Ioc` (the engine also has `PostOnly` / `Fok`
and a native `amend`, none used here). Not produced natively in the harness wire
format: `OrderAck`, the IOC-residual `CancelAck`, `ModifyAck`, and
`ModifyReject` — the adapter synthesises these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `submit_events` / `cancel_events` runs and
  emits inline, so `engine_flush` is a no-op and there is no drain step.
- Adapter state is two single-thread-owned globals (the transport vtable + sink,
  and the `OrderBook`) behind an `UnsafeCell` (`ThreadOwned`). The harness drives
  every `engine_on_*` / `engine_query_*` from one matcher thread, so there is no
  lock and no atomic on the hot path — the Rust expression of the C++ reference
  adapters' plain globals (same pattern as the orderbookrs / philipgreat
  adapters). No hot-path heap allocation beyond the engine's own per-call event
  `Vec`.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `u64` price (the engine's ticks are unsigned indices into the per-side maps).
- **New order**: `Order::limit` (or `Order::ioc` when the message's IOC flag is
  set). The event stream maps `Accepted -> OrderAck` and each `Trade ->`
  `ME_TRADE` (the taker is the `buy_id` for a Buy aggressor, the `sell_id` for a
  Sell aggressor; the price printed is the maker's resting price). `Rested`
  carries no harness report.
- **IOC**: the engine's native `Ioc` matches what it can and drops the residual
  internally with no event; the adapter derives the unfilled remainder from the
  fills it saw and emits the harness `CancelAck` for it (Trades first, then the
  CancelAck — the same emission order as the C++ reference adapters).
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost,
  and every canonical modify is a quantity *increase*, often a reprice). The
  engine's native `amend` is unsuitable — it rejects quantity increases and
  rejects crossing reprices (maker-only semantics) — so the adapter performs the
  contract literally: `cancel_events(id)`; on `Canceled` emit `ModifyAck` then
  re-`submit_events` a fresh GTC `Limit` at the new price/qty (any crossing fills
  emit as Trades tagged with the modify's seq; the reinsert's `Accepted` is
  dropped, so a modify yields exactly one `ModifyAck`, not an `OrderAck`); on
  `CancelRejected` emit `ModifyReject`. This is what the liquibook baseline and
  every reference adapter do.

## Source patch

`build.sh` applies one engine-source change after pinning. It is **not a
correctness fix** — gocronx is consensus-conforming as shipped
(`CORRECTNESS_FINDINGS.md`: gocronx, Rust, *No fix required*) — but an
**adapter-support accessor** the build needs to fill the CancelAck wire line.

**Add a read-only `OrderBook::get_order_se` accessor** (`src/book/mod.rs`): the
harness CancelAck wire line is `2,seq,side,order_id,price_ticks`, so a successful
cancel must echo the resting order's side **and** price. The engine's public
cancel API (`cancel_events`) returns only `Canceled{order_id, remaining, ts}` —
no side, no price — and the engine ships no public id→order getter (its `orders`
map is `pub(super)`). Rather than duplicate the book in an adapter-side shadow
(which could mask or invent an engine bug), the patch exposes a tiny read-only
accessor that reads the engine's own authoritative `orders` map:

```rust
pub fn get_order_se(&self, id: impl Into<OrderId>) -> Option<(Side, Price)> {
    self.orders.get(&id.into()).map(|o| (o.side, o.price))
}
```

It is injected immediately after the existing `best_ask()` getter and does
**not** change matching, book state, or any existing behavior — it only widens
the visibility of data the engine already holds. There is no filed upstream
issue because there is no behavioral defect; this is purely an adapter-visibility
shim. `build.sh` applies it with an anchored replace plus a marker guard and a
hard post-check, so an upstream change fails the build loudly rather than
silently shipping the wrong code; it is idempotent (the `git reset --hard`
restores pristine source each run on the default checkout, and the marker guard
makes a re-apply under an `ME_GOCRONX_SRC` override a no-op).

Reading the engine's own `orders` (rather than a shadow) is deliberate: it keeps
the adapter free of duplicated book state, so any resting / side / price
disagreement is attributable to the engine.

## Build / run

```bash
bash additional_references/gocronx_adapter/build.sh
./harness --engine gocronx_adapter.so --scenario normal --mode audit \
          --matcher-core 70 --drainer-core 71
```

`build.sh` clones the engine into `third_party/gocronx-matcher/` at the pinned
commit, applies the read-only accessor, then builds the engine + this adapter
into `gocronx_adapter.so` at the repository root. The `matcher` crate is one
library, so the whole crate (and its `tokio` / `socket2` deps, used by the
engine's UDP gateway but never on the adapter's hot path) compiles as a path
dependency; the wrapper manifest mirrors the engine's `[profile.release]`
(`lto = "thin"`, `codegen-units = 1`) so the engine is built the way its own
project builds it. The wrapper installs a stable Rust toolchain into
`$HOME/.cargo` only if `cargo` is not already on `PATH`. Override the checkout
with `ME_GOCRONX_SRC=/path/to/existing/clone` (the engine repo root, the dir
that contains `src/book/mod.rs`) to skip the clone.

> Note: the engine crate links `tokio` (for its production UDP gateway, unused
> here), so loading the `.so` spins up a couple of idle tokio background threads.
> The harness reports them as *informational* (`Engine threads: after_init 2`);
> they never run any matching — every `engine_on_*` call is handled synchronously
> on the single matcher thread — and the conformance verdict is unaffected.
