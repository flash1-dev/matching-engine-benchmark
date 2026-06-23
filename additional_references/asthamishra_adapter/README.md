# asthamishra_adapter — integration example

Wraps [AsthaMishra/matching-engine](https://github.com/AsthaMishra/matching-engine)
behind `api/matching_engine_api.h`. The engine is pure Rust; the entire adapter
is one Rust `cdylib` that exports the harness `engine_*` extern-C symbols and
calls straight into the engine's `matching-core` leaf crate. No C++ shim.

Pinned commit: `317c092843d3a5cc6730ceed6c56bb5598ab8fb7`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

`matching-core` is a single-symbol, single-threaded matcher. The book is a
**direct-indexed price array** — a price tick *is* the array index (`TICK_SIZE
= 1`, slots = `MAX_PRICE / TICK_SIZE`) — with an occupied-level bitmap and a
`Vec<Option<(side, price, qty, idx)>>` order index keyed by the order's own id.
Native APIs visible from the adapter:

- `OrderBook::new()` — constructs the single book.
- `matching::match_order(&mut book, Order, CommandType)` — synchronous; matches
  the incoming order against the book, rests a `Limit` residual or (IOC) drops
  it, and returns a `Vec<OrderEvent>`: one `Executed(Trade)` per fill in match
  order, plus at most one `Accepted`/`Replace`/`Rejected`. Each `Trade` carries
  the maker id, taker id, and the maker's resting price — exactly the harness
  Trade fields.
- `OrderBook::cancel_order(id)` — id-keyed; `Ok(OrderEvent::Canceled{..})` when
  the order was resting, `Ok(OrderEvent::Rejected{..})` when not (already filled
  / already cancelled / never seen).
- `OrderBook::get_order_by_id(id)` — `Option<&(Side, price, qty, idx)>`, read
  just before a cancel to recover the resting order's side+price for the
  CancelAck (the engine's `Canceled` event does not echo them).
- `OrderBook::best_bid()` / `best_ask()` — `Option<i64>` ticks.
- `OrderBook::volume_at_price(side, price)` — `Option<u64>` resting qty.

The engine keys every resting order by the caller's `usize` id in its own order
index, and the harness order id is a `u64` that fits in 32 bits, so the harness
id is used verbatim as the engine id and the engine's own index serves as the
adapter's liveness/reject oracle — **the adapter keeps no per-order shadow
state**. Rejects and the CancelAck side/price come from the engine itself.

Native order types: `Limit` and `IOC` (no FOK / POST-ONLY). There is a native
`matching::replace_order`, but its same-price / qty-decrease fast paths keep
queue priority, which deviates from the harness's modify contract (see below);
the adapter does not use it.

Not produced natively in the harness wire format: OrderAck, CancelAck (incl.
the IOC residual), ModifyAck, CancelReject, ModifyReject. The adapter
synthesises these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `match_order` / `cancel_order` runs and
  emits inline, so `engine_flush` is a no-op and there is no drain step.
- Adapter state is two single-thread-owned globals (the transport vtable + sink,
  and the `OrderBook`) behind an `UnsafeCell`. The harness drives every
  `engine_on_*` / `engine_query_*` from one matcher thread, so there is no lock
  and no atomic on the hot path — the Rust expression of the C++ reference
  adapters' plain globals (same pattern as the orderbookrs / philipgreat
  adapters). No hot-path heap allocation: `Order::new` is a stack value and the
  engine reuses its pre-sized arrays.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `i64` price (the engine's `TICK_SIZE = 1`, so a tick is an index).
- **IOC**: the engine drops any IOC residual internally; the adapter derives the
  unfilled remainder from the fills it saw and emits the harness `CancelAck`.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost).
  The adapter does `cancel_order` then a fresh `match_order` at the new
  price/qty, taking the resting order's *true* side from `get_order_by_id`
  (authoritative over the message's side field) — rather than the engine's
  `replace_order`, whose fast paths would keep priority.
- **Self-trade prevention**: the engine's matcher *cancels* a resting order that
  shares the incoming order's `trader_id` instead of crossing it (production
  STP), but the harness models a single anonymous flow with plain price-time
  priority and no STP. The adapter neutralises this by giving every order a
  **unique `trader_id` (= its order id)**, so two distinct orders never share a
  trader and the STP branch can never fire.

## Source patch

The shipped engine is patched for correctness. `build.sh` applies one
engine-source change after pinning; without it the engine is **INVALID** on the
benchmark's wide-swing tapes.

**Widen the direct-indexed price domain**
(`matching-core/src/utils.rs`): the book array is indexed by price and
`price_to_idx()` rejects any price `>= MAX_PRICE`, with `MAX_PRICE = 100_000`
(`TICK_SIZE = 1`). Orders priced at or above 100,000 ticks are therefore dropped
*before they enter the book* — they never rest and leave no trace — and a later
cancel/modify of such an id then rejects with `OrderIdNotFound`, so the engine
under-fills and its report stream diverges from consensus. The flash-crash
scenario breaches the ceiling (it reaches ~153k ticks on some seeds; e.g. seed
88 reaches ~104.5k and, unpatched, the engine logs thousands of `price exceeds
maximum supported value` rejections). This is the engine's
[issue #1](https://github.com/AsthaMishra/matching-engine/issues/1), whose
recommended option 1 is to widen the domain. The patch raises a single
constant — `MAX_PRICE` from `100_000` to `2_000_000` ($20,000 at 1-cent ticks,
~13× the worst tick observed across the tapes) — which the array-sizing
expression (`slots = MAX_PRICE / TICK_SIZE`) and the `price_to_idx` bound both
key off, so nothing else changes and the engine keeps its O(1) direct indexing.
The `price <= 0` and out-of-domain rejections in `price_to_idx` are kept, so a
genuinely out-of-range price still rejects distinctly. The array grows to
2,000,000 slots (~160 MB for the single benchmark book). `build.sh` applies the
change with an anchored replace plus a marker guard and a hard post-check, so an
upstream change fails the build loudly rather than silently shipping the bounded
engine; it is idempotent (the `git reset --hard` restores pristine source each
run).

This is an engine-source fix, not an adapter workaround: the adapter relies on
the engine actually holding every order (it uses the engine's id index as its
reject oracle), so the orders must enter the book rather than being filtered in
the shim.

## Build / run

```bash
bash additional_references/asthamishra_adapter/build.sh
./harness --engine asthamishra_adapter.so --scenario normal --mode audit \
          --matcher-core 54 --drainer-core 55
```

`build.sh` clones the engine into `third_party/asthamishra-matching-engine/` at
the pinned commit, applies the price-domain fix, then builds `matching-core` +
this adapter into `asthamishra_adapter.so` at the repository root. Only the
`matching-core` leaf crate (deps: `serde`) is compiled — the workspace's
`server` / `ouch-gateway` / `rest-gateway` crates (tokio / axum / crossbeam) are
not pulled in, because the adapter's wrapper manifest declares its own empty
`[workspace]`. The wrapper installs a stable Rust toolchain into `$HOME/.cargo`
only if `cargo` is not already on `PATH`. Override the checkout with
`ME_ASTHAMISHRA_SRC=/path/to/existing/clone` (the engine repo root, the dir that
contains `matching-core/`) to skip the clone.
