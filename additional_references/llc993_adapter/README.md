# llc993_adapter — integration example

Wraps [llc-993/matching-core](https://github.com/llc-993/matching-core) behind
`api/matching_engine_api.h`. The engine is pure Rust; the entire adapter is one
Rust `cdylib` that exports the harness `engine_*` extern-C symbols and calls
straight into the engine's `matching-core` crate. No C++ shim.

Pinned commit: `2cb21c0a67b34b01ad97e2394a649fc77e33aa8b`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` and
`CONSENSUS_CONFORMING_ENGINES.md` at the repository root for the verdict the
harness produced against this snapshot (conforming **as shipped** — no fix
required).

## Engine shape

`matching-core` is an exchange-core-inspired matcher. The engine under test is
its production / default order book, **`DirectOrderBook`** — the book
`MatchingEngineRouter::add_symbol` instantiates and the headline implementation
in the engine's own `exchange_bench`. It is a price-time-priority CLOB:

- a `BTreeMap` from price to a price-bucket index,
- a `Slab` order pool with an **intrusive doubly-linked time queue** per level,
- an `AHashMap` order-id index, and
- cached best-bid / best-ask order handles.

The full engine ships a Disruptor pipeline + risk/journal/snapshot stages around
this book, but that machinery only moves commands across threads — no matching
logic lives there. The adapter drives the book directly through its native
`OrderBook` trait, which *is* the matcher, so this exercises the engine's real
matching code path. Native APIs visible from the adapter:

- `DirectOrderBook::new(spec)` — constructs the single-symbol book from a
  `CoreSymbolSpecification` (the adapter uses a zero-fee, unit-scale
  `CurrencyExchangePair`).
- `OrderBook::new_order(&mut OrderCommand)` — synchronous; matches the incoming
  order against the book, rests a `Gtc` residual or (`OrderType::Ioc`) drops it.
  Each fill is pushed onto `cmd.matcher_events` as a `MatcherTradeEvent`
  (`event_type: Trade`, `size`, `price` = the **maker's** resting price,
  `matched_order_id` = the maker id); the taker id is `cmd.order_id`.
- `OrderBook::cancel_order(&mut OrderCommand)` — natively id-keyed; returns
  `Success` when the order was resting (and echoes the order's side into
  `cmd.action`) or `MatchingUnknownOrderId` when it was not (already filled /
  cancelled / never seen).
- `OrderBook::get_order_by_id(id) -> Option<(Price, OrderAction)>` — the native
  resting test, read just before a cancel/modify to recover the order's
  side+price for the CancelAck / ModifyAck and to make the reject decision.
- L2 readout: `DirectOrderBook`'s fields are private, but the `OrderBook` trait
  exposes `get_l2_data(depth) -> L2MarketData` (bids highest-first, asks
  lowest-first, with parallel volume vectors). Best bid/ask and per-price depth
  are read from it.

The engine keys every resting order by the caller's id in its own `AHashMap`
index, and the harness order id is used verbatim, so **the engine's own index
is the adapter's liveness/reject oracle — the adapter keeps no per-order shadow
state**. Rejects and the CancelAck/ModifyAck side+price come from the engine
itself.

Native order types used: `Gtc` and `Ioc`. The engine also has a native
`move_order`, but the adapter does **not** use it (see Adapter strategy).

Not produced natively in the harness wire format: OrderAck, CancelAck (incl. the
IOC residual), ModifyAck, CancelReject, ModifyReject. The adapter synthesises
these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `new_order` / `cancel_order` runs and emits
  inline, so `engine_flush` is a no-op and there is no drain step.
- Adapter state is two single-thread-owned globals (the transport vtable + sink,
  and the `DirectOrderBook`) behind an `UnsafeCell`. The harness drives every
  `engine_on_*` / `engine_query_*` from one matcher thread, so there is no lock
  and no atomic on the hot path — the Rust expression of the C++ reference
  adapters' plain globals (same pattern as the orderbookrs reference adapter).
  No hot-path heap allocation: each `OrderCommand` is a stack value and the
  engine reuses its pre-sized slab/queue.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `i64` `Price`.
- **IOC**: the engine drops any IOC residual internally; the adapter sums the
  fills it saw, derives the unfilled remainder, and emits the harness
  `CancelAck` for it.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost).
  The adapter does `cancel_order` then a fresh `new_order` (`Gtc`) at the new
  price/qty on the resting order's **true** side, taken from `get_order_by_id`
  (authoritative over the message's side field). It deliberately does **not**
  use the engine's `move_order`: that path applies a reserve-price risk check
  (`cmd.price > order.reserve_price` rejects a bid move) and, with
  `reserve_price` defaulted to 0, every bid reprice-up would be rejected — not
  the harness's cancel+reinsert semantics. cancel+reinsert is exactly what every
  reference adapter does.
- **Audit queries**: best bid/ask and per-price depth are answered by walking
  `get_l2_data(depth)` with a depth wide enough to cover the workload's price
  band (the trait exposes no direct best-bid / price-keyed-depth getter).

## Source patch

No source patch. The engine compiles and runs against the harness as shipped at
the pinned commit, and reaches byte-for-byte consensus with the reference
engines (conforming **as shipped** — see `CONSENSUS_CONFORMING_ENGINES.md` /
`CORRECTNESS_FINDINGS.md`). `build.sh` clones the engine at the pin and builds
it unmodified; there is no upstream issue to cite.

## Build / run

```bash
bash additional_references/llc993_adapter/build.sh
./harness --engine llc993_adapter.so --scenario normal --mode audit \
          --matcher-core 60 --drainer-core 61
```

`build.sh` clones the engine into `third_party/llc993_matching_core/` at the
pinned commit, then builds `matching-core` + this adapter into
`llc993_adapter.so` at the repository root. The wrapper's manifest declares its
own empty `[workspace]`, so `cargo build` compiles only the engine crate (and
its dependencies), nothing above it in the tree. The wrapper installs a stable
Rust toolchain into `$HOME/.cargo` only if `cargo` is not already on `PATH`.
Override the checkout with `ME_LLC993_SRC=/path/to/existing/clone` (the engine
repo root, the dir that contains `src/` and `Cargo.toml`) to skip the clone.
