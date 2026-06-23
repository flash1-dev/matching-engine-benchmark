# pgellert_adapter — integration example

Wraps [pgellert/matching-engine](https://github.com/pgellert/matching-engine)
behind `api/matching_engine_api.h`. The engine is pure Rust; the entire adapter
is one Rust `cdylib` that exports the harness `engine_*` extern-C symbols and
drives the engine's production matcher
(`engine::algos::optimised_fifo::FIFOBook` — the book
`engine/src/rpc/me_state_machine.rs` constructs). No C++ shim.

Pinned commit: `de195a8227b942f10fd5cb41934d1ce325dd8dd9`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

`FIFOBook` is a price-time-priority matcher built as a Raft state machine: a
`HashMap<price, VecDeque<Order>>` per side, cached `min/max` price bounds used as
a scan window, and an `orders: HashMap<(client_id, seq_number), price>` index.
The `Book` trait it implements is **symmetric / batch-style**, not
aggressor-driven:

- `apply(order)` — only *rests* the order in its price bucket; it never matches.
- `check_for_trades() -> Vec<Trade>` — repeatedly pops the best bid + best ask
  and `merge`s them while they cross, returning every `Trade { ask, bid,
  quantity }` produced. A `Trade` carries the full resting `Order` on each side
  but no taker/maker tag — the engine state machine drains crossings after every
  `apply`, so the only order that can be crossing is the one just applied.
- `cancel(id, side) -> bool` — removes the order from its bucket; **requires the
  side as an input** and returns only a boolean.
- The vendored matcher additionally exposes three READ-ONLY accessors added for
  the harness audit — `best_bid()` / `best_ask()` / `depth_at(price, side)` —
  which scan the live buckets and touch none of the matcher state (see the
  *Source patch* / *Vendoring* notes below).

The engine keys an order by `(client_id, seq_number)`. The harness identifies an
order by a single `order_id`, so the adapter maps it to the engine key `(0,
order_id)` and recovers the harness id as the `seq_number`. The harness
`sequence_number` is a *separate* field used only to stamp reports (the
aggressor's seq); it is carried out-of-band and never used as the engine key.

Native order types: plain limit only — **no IOC / FOK / POST-ONLY**, and **no
native modify**. Not produced natively in the harness wire format: OrderAck,
CancelAck (incl. the IOC residual), ModifyAck, CancelReject, ModifyReject, and
the maker/taker-tagged Trade. The adapter synthesises these around the engine
calls.

## Adapter strategy

- The book is driven exactly as the engine's `MEStateMachine` drives it
  (`me_state_machine.rs:97-99`): for each aggressor message, `book.apply(order)`
  then `book.check_for_trades()`. No matching logic lives in the adapter.
- The matcher is synchronous: every message applies, matches, and emits inline,
  so `engine_flush` is a no-op and there is no drain step.
- Adapter state is two single-thread-owned globals (the transport vtable + sink,
  and the `State { book, shadow }`) behind an `UnsafeCell`. The harness drives
  every `engine_on_*` / `engine_query_*` from one matcher thread, so there is no
  lock and no atomic on the hot path — the Rust expression of the C++ reference
  adapters' plain globals (same pattern as the orderbookrs adapter). No hot-path
  heap allocation on the steady-state path: orders are stack values and the
  liveness shadow is pre-reserved once in `engine_init`.
- **Trade tagging / fill price**: because the book drains every cross after each
  `apply`, the order just applied is the aggressor (taker) and every order it
  trades against is resting (a maker). The adapter resolves maker vs taker from
  the *known* aggressor side and stamps each symmetric `Trade`: aggressor Buy ⇒
  taker is `bid`, maker is `ask`, fill price = `ask.price`; aggressor Sell ⇒
  taker is `ask`, maker is `bid`, fill price = `bid.price`. (The engine's own
  state machine instead reports `trade.bid.price` for *every* fill —
  `me_state_machine.rs:103` — which is not the maker's resting price for a Sell
  aggressor; the adapter emits the correct maker price, since the harness wire
  format demands it. This is a *report-mapping* correction in the adapter, not an
  engine change.)
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `u64` price (harness ticks are non-negative).
- **IOC**: the engine has no IOC flag. The order is submitted as a normal limit,
  matched, and any resting residual is then `cancel`ed and dropped with exactly
  one `CancelAck` for the unfilled remainder — the residual-cancel pattern the
  harness template prescribes.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost).
  The adapter `cancel`s the old order on its recorded side, emits one
  `ModifyAck`, then submits a fresh limit at the new price/qty on the same side
  (which may cross and produce Trades).
- **Per-order liveness shadow**: `cancel(id, side)` *requires* the side and
  returns only `bool`, while the harness CancelAck/ModifyAck lines echo the
  resting order's side + price and the CancelReject/ModifyReject decision needs
  to know whether the id is resting. So the adapter keeps a minimal shadow
  (`side, price, resting qty, alive`) keyed by order id — the same shadow the
  liquibook / quantcup reference adapters keep for the same reason — to drive the
  side argument, the echoed ack fields, and the reject-vs-ack decision. The
  shadow is decremented as a maker fills and cleared when the order is fully
  filled or cancelled; a cancel/modify of an id with no live shadow entry
  (already filled, already cancelled, or never resting) rejects.

`engine_prebuild` is **not** exported: this adapter does no prebuild pass, so
there is nothing for the harness's post-prebuild empty-book assertion to catch.

## Source patch

The shipped engine matcher is patched for correctness. `build.sh` applies one
engine-source change — to `engine/src/algos/optimised_fifo.rs` only — after
pinning; without it the engine is **INVALID** on the benchmark tapes (it
under-matches by roughly 70%). The patch is the engine's
[issue #2](https://github.com/pgellert/matching-engine/issues/2); it fixes two
hard-invariant matcher bugs.

**1. Conserve a popped-but-unconsumed order.** `check_for_trades` calls
`pop_bid` / `pop_ask`, which *remove* the front order from its bucket before the
loop decides whether the pair trades. On the exit paths where a popped order does
not lead to a trade — one side empty, or (after the bounds fix below) a popped
pair that does not actually cross — the original code does not put the order
back, so resting liquidity is silently destroyed (quantity non-conservation).
The original code only `push_front`s a *partial-fill remainder*, and never the
whole popped order. The patch adds `unpop()` (re-insert at the **front** of the
bucket, restoring FIFO position) and routes every non-trading / empty exit
through it.

**2. Correct the stale price bounds.** `pop_bid` / `pop_ask` ratchet the cached
`min/max` bound to *every price they scan over* (including empty buckets), and
`apply()` only refreshes the bound on the path that creates a **new** bucket — so
an order landing in a previously-emptied bucket cannot widen the live-best bound
back out. The bounds drift stale-permissive: the cross guard lets a non-crossing
pair through, and a genuinely marketable order can be hidden behind a stale
bound, so it never matches. The patch (a) commits the pop bound only to a price
that *actually holds a live order*, (b) refreshes the bound on **every** insert
in `apply()`, and (c) re-checks the real cross condition (`ask.price <=
bid.price`) inside the match loop, `unpop`ing both orders and stopping when the
popped pair does not actually cross — which is what makes leaving the bounds
permissive safe.

This is an engine-source fix, not an adapter workaround: the adapter drives the
vendored matcher exactly as the engine's `MEStateMachine` does (`apply` →
`check_for_trades` per message), so the conservation and price-priority
invariants have to hold *in the matcher itself*. The patch touches **only**
`optimised_fifo.rs`; `book.rs` (Order, Side, Trade, `Order::merge`, the Buy/Sell
`Ord` impls, the `Book` trait) and every other engine file are untouched.
`build.sh` applies the change with `git apply` against the freshly hard-reset pin
(git's own context matching is the anchor guard, so a drifted upstream fails the
build loudly), with a hard post-check for `fn unpop`, and idempotently (the
`git reset --hard` restores pristine source each default run; an already-patched
override checkout is detected and skipped). `wrapper/examples/verify_fix.rs`
asserts both bugs are gone on the patched matcher (run from `wrapper/`:
`cargo run --release --example verify_fix`).

### Vendoring (why this adapter ships engine source under `wrapper/src/algos/`)

`FIFOBook` needs nothing but `std` collections, but the engine crate that defines
it is a multi-crate Raft workspace whose `engine/` crate pulls in
tonic / prost / tokio 0.2 / a git-pinned ART crate and a protobuf-codegen
`build.rs` — none of which the matcher uses. So the wrapper **vendors** exactly
the two source files the matcher lives in, committed under
`wrapper/src/algos/`, and builds them with no external dependency:

- `wrapper/src/algos/optimised_fifo.rs` — the patched
  `engine/src/algos/optimised_fifo.rs` verbatim, **minus** the trailing
  `#[cfg(test)]` module (it uses the engine's `rand` dev-dependency) and
  **plus** the three appended read-only audit accessors. The matcher state
  machine (`apply` / `check_for_trades` / `cancel`) is byte-identical to the
  patched engine source.
- `wrapper/src/algos/book.rs` — `engine/src/algos/book.rs` with **only**
  `use crate::protobuf;` and the three protobuf/raft bridge methods
  (`from_proto` / `into_proto` / `from_command`) removed. The matching logic is
  unchanged from upstream.

`build.sh` does **not** regenerate the vendored copies; it clones the pinned
engine, applies the issue #2 patch, and then **verifies** the committed vendored
matcher is identical to the patched engine source (a structural diff that ignores
only the documented test-strip / protobuf-strip / accessor differences). The
vendoring therefore can never silently diverge from the pin, and the matcher that
ships is exactly the pinned engine matcher **plus** the issue #2 fix.

## Build / run

```bash
bash additional_references/pgellert_adapter/build.sh
./harness --engine pgellert_adapter.so --scenario normal --mode audit \
          --matcher-core 66 --drainer-core 67
```

`build.sh` clones the engine into `third_party/pgellert-matching-engine/` at the
pinned commit, applies the issue #2 matcher fix, verifies the vendored matcher
against the patched pin, then builds the wrapper crate into `pgellert_adapter.so`
at the repository root. Only the wrapper's vendored matcher (deps: none — `std`
only) is compiled; the engine workspace's Raft / gRPC server crates
(tonic / prost / tokio / the ART dependency) are **not** pulled in, because the
wrapper manifest declares its own empty `[workspace]`. The wrapper installs a
stable Rust toolchain into `$HOME/.cargo` only if `cargo` is not already on
`PATH`. Override the checkout with `ME_PGELLERT_SRC=/path/to/existing/clone` (the
engine repo root, the dir that contains `engine/src/algos/`) to skip the clone.
