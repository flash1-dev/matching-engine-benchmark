# lobster_adapter — integration example

Wraps [rubik/lobster](https://github.com/rubik/lobster) behind
`api/matching_engine_api.h`. The engine is pure Rust; the entire adapter is one
Rust `cdylib` that exports the harness `engine_*` extern-C symbols and calls
straight into lobster's public `OrderBook` API. No C++ shim.

Pinned commit: `0b9720ca1e7dd1f81ecd35d1062c0d3044d5607d`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

`lobster::OrderBook` is a single-symbol, single-threaded matcher: a `BTreeMap`
of price levels with a slab arena for the resting orders and a `u128`-keyed
order index (`OrderArena.order_map`). Native APIs visible from the adapter:

- `OrderBook::new(arena_capacity, queue_capacity, track_stats)` — constructs
  the book with pre-sized arena/queue capacity.
- `OrderBook::execute(OrderType) -> OrderEvent` — synchronous price-time
  matcher. For a `Limit` it matches the incoming order against the book and
  rests any residual, returning an `OrderEvent` (`Placed` / `PartiallyFilled` /
  `Filled` / `Unfilled`) plus, on a (partial) fill, a `Vec<FillMetadata>` — one
  entry per fill in match order. Each `FillMetadata` carries `order_1` (taker
  id), `order_2` (maker id), `price` (the maker's resting price), `qty`, and
  `total_fill` (whether this fill fully consumed the maker). That fill vector
  **is** the harness Trade stream, field for field.
- `OrderBook::execute(OrderType::Cancel { id })` — removes the order by its
  caller-supplied `u128` external id.
- `OrderBook::min_ask()` / `max_bid()` — `Option<u64>` best ask / bid ticks.
- `OrderBook::depth(levels)` — a `BookDepth` of every non-empty price level on
  both sides (the `levels` argument only pre-sizes the result `Vec`; the walk
  covers the whole book regardless).

lobster keys every resting order by the caller's `u128` external id and echoes
that same id back in every `FillMetadata`. The harness order id is a `uint64_t`
that widens losslessly to `u128`, so the harness id is used verbatim as the
engine id and trade reports recover the harness ids directly — no sidecar
translation map.

Native order types: `Limit`, `Market`, `Cancel`. There is no native IOC / FOK /
POST-ONLY and no native modify.

Not produced natively in the harness wire format: OrderAck, CancelAck (incl. the
IOC residual), ModifyAck, CancelReject, ModifyReject. The adapter synthesises
these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `execute()` runs and emits inline, so
  `engine_flush` is a no-op and there is no drain step.
- Adapter state is single-thread-owned globals (the transport vtable + sink, the
  `OrderBook`, and the liveness shadow below) behind an `UnsafeCell`. The
  harness drives every `engine_on_*` / `engine_query_*` from one matcher thread,
  so there is no lock and no atomic on the hot path — the Rust expression of the
  C++ reference adapters' plain globals (same pattern as the orderbookrs /
  philipgreat adapters). No hot-path heap allocation: the engine's arena/queue
  and the shadow vector are pre-sized once in `engine_init`.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `u64` price (harness ticks are non-negative, so the widen is lossless).
- **IOC**: lobster has no native immediate-or-cancel. A harness IOC new order is
  submitted as a plain `Limit` — which matches what it can and rests the
  residual — and the adapter then removes that residual with a follow-up
  `Cancel { id }` (its event discarded) and emits the harness IOC-residual
  `CancelAck`. An IOC id is never recorded live, so it never rests in the
  harness view.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost).
  The adapter does `Cancel { id }` then a fresh `Limit { id, new_price, new_qty }`
  on the same side, so any crossing fills on the reinsert emit as Trades,
  followed by one `ModifyAck`.

## Liveness shadow

lobster's public `execute(Cancel { .. })` **always** returns
`OrderEvent::Canceled`, even for an id that is not resting (already filled,
already cancelled, or never seen) — its internal not-found bool is dropped
before it reaches the public API. It also does not echo the cancelled order's
side or price. The harness needs `CancelReject` / `ModifyReject` for exactly the
not-resting cases, and the canonical `CancelAck` line carries the resting
order's side+price.

So the adapter keeps one minimal per-order shadow — a flat `Vec<Shadow>` indexed
by the dense harness order id, each entry `{ live, side, price }`. An id is set
live (with its side+price) when a GTC limit rests a residual, and cleared when
it is fully consumed by a later incoming order (`FillMetadata.total_fill`),
cancelled, or modified. This is the **only** adapter-side order state; it exists
solely because the engine's public API cannot report not-resting nor echo the
cancelled order's side/price. The book itself is the engine's — the adapter does
not rebuild it. The shadow is pre-sized to the dense id range in `engine_init`
(capacity only, untimed); the steady-state path only reads/sets flags.

## Source patch

No source patch. lobster is conforming as shipped — `CORRECTNESS_FINDINGS.md`
records "No fix required" — so `build.sh` builds the engine exactly as upstream
ships it at the pinned commit (`git reset --hard` to the SNAPSHOTS pin, then
`cargo build`). There is no filed upstream issue for this engine.

## Build / run

```bash
bash additional_references/lobster_adapter/build.sh
./harness --engine lobster_adapter.so --scenario normal --mode audit \
          --matcher-core 62 --drainer-core 63
```

`build.sh` clones the engine into `third_party/lobster/` at the pinned commit,
then builds `lobster` + this adapter into `lobster_adapter.so` at the repository
root. lobster has no dependencies of its own (its Cargo.toml lists only
dev-dependencies, which a plain `cargo build` does not pull in), and the
wrapper's manifest declares its own empty `[workspace]` so the build stays
self-contained. The wrapper installs a stable Rust toolchain into `$HOME/.cargo`
only if `cargo` is not already on `PATH`. Override the checkout with
`ME_LOBSTER_SRC=/path/to/existing/clone` (the engine repo root, the dir that
contains `src/orderbook.rs`) to skip the clone.
