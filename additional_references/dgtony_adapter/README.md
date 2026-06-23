# dgtony_adapter — integration example

Wraps [dgtony/orderbook-rs](https://github.com/dgtony/orderbook-rs) behind
`api/matching_engine_api.h`. The engine is pure Rust; the entire adapter is one
Rust `cdylib` that exports the harness `engine_*` extern-C symbols and calls
straight into the engine's `orderbook` crate. No C++ shim.

Pinned commit: `cba8329b1f6cb2156c734b4cfab8ab0cc5566cc6`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

`orderbook` is a single-threaded reactor: one currency pair, consuming one
request and returning the events it generated. Its entire public surface is

```rust
Orderbook::process_order(OrderRequest) -> Vec<Result<Success, Failed>>
```

Each side is a price-time-priority `BinaryHeap` index over a side
`HashMap<id, Order>` (`src/engine/order_queues.rs`). Native APIs visible from
the adapter:

- `Orderbook::new(order_asset, price_asset)` — constructs the single book; the
  engine is generic over an asset enum, so the adapter uses a single-variant
  asset and passes it for both sides.
- `process_limit_order` / `process_order_cancel` — the engine's own (private)
  matching helpers; the adapter reaches them through two thin pub wrappers added
  by the build-time patch (see **Source patch** below) that bypass **only** the
  engine's id generator, nothing in the matching path.
- `bid_queue.peek()` / `ask_queue.peek()` — the heap top, with the engine's own
  lazy stale-index cleanup; surfaced as `best_bid()` / `best_ask()`.
- a sum over the side queue's live `orders` map — surfaced as `depth_at(price,
  side)`.

Native order types: market and limit, plus a native `AmendOrder` and
`CancelOrder`. There is no IOC / FOK / POST-ONLY time-in-force, and the engine
emits none of the harness wire-format reports.

Not produced natively in the harness wire format: OrderAck, CancelAck (incl. the
IOC residual), ModifyAck, CancelReject, ModifyReject. The adapter synthesises
these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `submit_limit` / `submit_cancel` runs and
  emits inline, so `engine_flush` is a no-op and there is no drain step.
- Adapter state is three single-thread-owned globals (the transport vtable +
  sink, the `Orderbook`, and the liveness shadow) behind an `UnsafeCell`. The
  harness drives every `engine_on_*` / `engine_query_*` from one matcher thread,
  so there is no lock and no atomic on the hot path — the Rust expression of the
  C++ reference adapters' plain globals (same pattern as the orderbookrs
  adapter). No hot-path heap allocation beyond the engine's own event `Vec` and
  the shadow's amortised insert.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `f64` price; fills are rounded back to `i64` ticks (the tapes are integer
  ticks, so the round-trip is exact).
- **Order ids**: the harness 32-bit id is threaded verbatim into the engine via
  `submit_limit` / `submit_cancel`, so the engine rests every order under the
  caller's id (see **Source patch**).
- **IOC**: the engine has no IOC time-in-force — an IOC order is matched as a
  plain limit and any rested residual is then cancelled back out of the engine
  and reported as the harness `CancelAck` (the documented "match what you can,
  drop the rest"). An IOC never rests, so it is never recorded in the shadow.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost,
  and a reprice that now crosses re-matches). The adapter does `submit_cancel`
  then a fresh `submit_limit` at the new price/qty, taking the resting order's
  side from the shadow. This routes the reprice through the engine's normal
  matching path, so it also sidesteps the engine's native-`AmendOrder` crossing
  bug (which does not re-match — see **Source patch** / issue #9) by never
  calling `AmendOrder`.
- **Liveness shadow** (the one piece of permitted per-order adapter state):
  `order_id -> (side, price)` for every **resting** order. The engine's
  `process_order_cancel` needs the order's *side* to pick a queue, and its
  `Cancelled` event carries only the id+ts — but the harness CancelAck line
  needs side + price, which `cancel_t` does not supply. So the shadow holds the
  side argument for the engine cancel and the side/price echo for the ack. The
  **engine stays authoritative for liveness**: the shadow is only ever a
  superset, reconciled from the fill events (a maker reported `Filled`, i.e.
  exhausted, is dropped), and a cancel/modify the engine answers with anything
  other than `Cancelled` is rejected regardless of the shadow.

## Source patch

The shipped engine is patched for correctness. `build.sh` applies one
engine-source change after pinning; without it the engine is **INVALID** on the
benchmark workload (it would rest at most ~1000 orders — see below).

This is the engine's
[issue #9](https://github.com/dgtony/orderbook-rs/issues/9), which reports two
latent defects. The patch is **minimal and additive** — it only *adds* new pub
methods that reuse the engine's existing (private) matching helpers; **no
existing engine line is changed**, so the matching / resting / cancel path stays
byte-for-byte the upstream behaviour.

**1. Caller-supplied order ids (the load-bearing fix).** `process_order` assigns
ids from an internal `TradeSequence` that **rotates in `[1, 1000]`**
(`MIN_SEQUENCE_ID = 1` / `MAX_SEQUENCE_ID = 1000`, `src/engine/orderbook.rs`;
`TradeSequence::next_id` wraps `1000 -> 1`, `src/engine/sequence.rs`) and ignores
the caller. The generated id is never collision-checked, so once 1000 orders are
live a wrapped id lands on a still-resting order and `OrderQueue::insert`
silently refuses the duplicate — the engine pushes `Success::Accepted` and then
`Failed::DuplicateOrderID` for the same order, which rests nowhere. The practical
effect is a hard ~1000-live-order cap, and the benchmark drives 300k–1M
messages, so unpatched the engine drops almost everything and its report stream
diverges wildly from consensus. The patch adds `Orderbook::submit_limit(id, …)`
and `Orderbook::submit_cancel(id, side)`, which thread the **harness** id
straight into the engine's own `process_limit_order` / `process_order_cancel`,
so the rotating generator and its `[1, 1000]` validation bound are never on the
path. issue #9 recommends widening the id space upstream; threading the caller
id is the adapter-build expression of the same fix and is what lets the engine's
own id index serve as the liveness/reject oracle.

**2. `AmendOrder` leaves the book crossed.** issue #9's second defect: the
engine's native `AmendOrder` rewrites the order in its own side and rebuilds that
side's heap but never consults the opposite side or calls the matcher, so
repricing a resting order *through* the top of book produces no trade and leaves
the book crossed. The adapter does **not** use `AmendOrder` — modify is cancel +
reinsert through `submit_limit` (the harness contract), which re-matches a
crossing reprice correctly — so this defect is structurally avoided rather than
patched. It is documented here because the same issue carries it.

**Query accessors (API gaps, not bugs).** The harness queries each side of the
book independently and asks for aggregated depth at a price, neither of which the
engine exposes directly (`current_spread()` returns `None` unless **both** sides
are populated). The patch adds `best_bid()` / `best_ask()` as a `peek()` of one
side, `depth_at(price, side)` as a sum over the side queue's live `orders` map,
and the `OrderQueue::iter_orders()` pub iterator that `depth_at` needs (the
`orders` field is private to `order_queues.rs`). These are read-only and touch no
matching state.

`build.sh` applies the change with anchored Python replaces plus per-method
marker guards and a hard post-check (it greps every injected `pub fn` and fails
the build if any is absent), so an upstream change fails loudly rather than
silently shipping the rotating-id engine; it is idempotent (the `git reset
--hard` restores pristine source each run, and a re-apply under an
`ME_DGTONY_SRC` override is a no-op).

This is an engine-source fix, not an adapter workaround: the adapter relies on
the engine actually holding every order under the caller's id (it uses the
engine's own cancel result as its liveness/reject oracle), so the orders must
enter the book under that id rather than being renumbered by the generator.

## Build / run

```bash
bash additional_references/dgtony_adapter/build.sh
./harness --engine dgtony_adapter.so --scenario normal --mode audit \
          --matcher-core 62 --drainer-core 63
```

`build.sh` clones the engine into `third_party/dgtony-orderbook-rs/` at the
pinned commit, applies the source patch, then builds the `orderbook` crate +
this adapter into `dgtony_adapter.so` at the repository root. The wrapper
installs a stable Rust toolchain into `$HOME/.cargo` only if `cargo` is not
already on `PATH`. Override the checkout with `ME_DGTONY_SRC=/path/to/existing/
clone` (the engine repo root, the dir that contains `src/engine/orderbook.rs`)
to skip the clone.
