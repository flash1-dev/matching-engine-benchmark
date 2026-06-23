# techieboy_adapter — integration example

Wraps [TechieBoy/rust-orderbook](https://github.com/TechieBoy/rust-orderbook)
(crate `orderbook`, lib `orderbooklib`) behind `api/matching_engine_api.h`. The
engine is pure Rust; the entire adapter is one Rust `cdylib` that exports the
harness `engine_*` extern-C symbols and calls straight into the engine crate by
path. No C++ shim.

Pinned commit: `468fef7fb86c6191d8a2fb4c4ad1d9fb88ec0a26`.

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

A single-symbol, single-threaded price-time-priority CLOB. Each side is a
`HalfBook` = a `BTreeMap<price, level_index>` over a `Vec<VecDeque<Order>>` (one
FIFO queue per occupied price level); a `HashMap<order_id, (Side, level_index)>`
(`order_loc`) indexes resting orders for cancels, and `best_bid_price` /
`best_offer_price` cache the BBO. `add_limit_order` crosses the incoming order
against the opposite book in strict price-then-time order and rests any residual;
`cancel_order` removes by id. It is a real matcher that crosses and produces fills
— not a depth-only structure. Native APIs visible from the adapter (after the
small build-time source patch — see **Source patch** below):

- `OrderBook::new(symbol)` — constructs the single book.
- `OrderBook::add_limit_order(side, price, qty, order_id)` — synchronous; matches
  the incoming order against the opposite book in price-then-time priority, then
  rests any residual at `price` keyed by `order_id`. Returns a `FillResult` whose
  patched `maker_fills` field lists one `(maker_order_id, fill_qty, maker_price)`
  per maker consumed, in match order, and whose `remaining_qty` is the taker's
  unfilled remainder. (Upstream minted the id internally with `rand` and recorded
  only a per-price-level `(qty, price)` aggregate; the patch threads the caller's
  id in and stops discarding the per-maker ids — the matching logic is unchanged.)
- `OrderBook::cancel_order(order_id)` — synchronous, natively id-keyed via the
  engine's own `order_loc` index. `Ok((side, price, qty))` = was resting and is
  now removed (the engine's own record of the order's fields); `Err(())` = not
  resting (already filled / cancelled / never seen). This is the native existence
  signal that drives CancelReject / ModifyReject.
- `OrderBook::eng_best_bid_raw()` / `eng_best_ask_raw()` — the engine's own cached
  BBO fields, returned verbatim (`u64::MIN` / `u64::MAX` empty sentinels).
- `OrderBook::eng_depth_at(price, side)` — live aggregated resting qty at one
  price level (0 if no such level).

The engine keys every resting order by the caller's `u64` id in its own
`order_loc` index, and the harness order id is a `u64`, so the harness id is used
verbatim as the engine id and the engine's own index serves as the adapter's
liveness/reject oracle — **the adapter keeps no per-order shadow state**. Rejects
and the CancelAck side/price come from the engine itself.

Native order types: plain `Limit` only (no IOC / FOK / POST-ONLY; no native
modify). Not produced natively in the harness wire format: OrderAck, CancelAck
(incl. the IOC residual), ModifyAck, CancelReject, ModifyReject. The adapter
synthesises these around the engine calls.

## Adapter strategy

- The matcher is synchronous: every `add_limit_order` / `cancel_order` runs and
  the adapter emits its reports inline, so `engine_flush` is a no-op and there is
  no drain step.
- Adapter state is two single-thread-owned globals (the transport vtable + sink,
  and the `OrderBook`) behind an `UnsafeCell`. The harness drives every
  `engine_on_*` / `engine_query_*` from one matcher thread, so there is no lock
  and no atomic on the hot path — the Rust expression of the C++ reference
  adapters' plain globals (same pattern as the orderbookrs / asthamishra
  adapters). The engine's methods take `&mut self`, so the wrapper exposes
  `get_mut`. No hot-path heap allocation in the adapter: the report structs are
  stack values and the engine reuses its own `VecDeque` / `HashMap` storage.
- **Report ordering** per new order: OrderAck first, then one Trade per maker in
  match order, then (IOC only) the residual CancelAck — the harness canonical
  order.
- **Prices**: workload `int64_t` ticks pass straight through as the engine's
  `u64` price (the harness ticks for these scenarios are non-negative; a negative
  is clamped to 0 defensively so the `u64` cast is lossless). The empty-book BBO
  sentinels are translated at the query boundary (`u64::MIN` → `INT64_MIN`,
  `u64::MAX` → `INT64_MAX`).
- **IOC**: the engine has no native IOC. The adapter submits the order as a plain
  limit (which matches what it can and rests the residual), then — for IOC only —
  cancels the just-rested residual by id and reports it as the harness
  IOC-residual CancelAck (`fr.remaining_qty` is the unfilled remainder). No
  residual ever survives an IOC.
- **Modify** = cancel + reinsert (the harness contract: queue priority is lost).
  The adapter does `cancel_order(id)` as the resting test (`Err` → ModifyReject),
  emits the ModifyAck, then a fresh `add_limit_order` on the message's side at the
  new price/qty, emitting any crossing fills as Trades (after the ModifyAck) and
  resting any residual under the same id. The engine exposes no native modify.

## Source patch

The shipped engine is patched for correctness and to expose its own behaviour to
the adapter. `build.sh` applies `patch_engine.py` after pinning (`git reset
--hard` to the pinned SHA first, so the patch always lands on pristine source);
each replacement is anchored and asserts it matched exactly once, so an upstream
drift fails the build loudly rather than silently shipping the wrong engine, and
the reset makes a rerun idempotent. Two genuine bug fixes are applied: one is
filed upstream as [issue #1](https://github.com/TechieBoy/rust-orderbook/issues/1);
the other (P5, the spurious zero-quantity fill) is documented below but is not
separately filed upstream. Without them the engine is **INVALID** on the benchmark
(see "Why these are load-bearing" below).

The patches, none of which change the matching algorithm except P5 (a bug fix):

| # | What | Why |
|---|------|-----|
| P1 | `create_new_limit_order` / `add_limit_order` take a caller-supplied `order_id` (the internal `rand::thread_rng().gen()` is dropped). | The harness owns order ids and must key cancel/modify by them natively. Upstream minted a random id internally and returned it, so a caller could not address its own resting order. |
| P2 | `FillResult` gains `maker_fills: Vec<(maker_id, qty, price)>`; `match_at_price_level` records one entry per maker consumed (and the call sites pass the level price + the buffer). | Upstream recorded only a per-price-level `(total_qty, price)` aggregate and discarded the individual maker ids inside `match_at_price_level`. The harness needs one Trade per maker carrying that maker's id. The matching logic is untouched — it just stops throwing the ids away. |
| P3 | Public read accessors `eng_best_bid_raw` / `eng_best_ask_raw` (the cached BBO fields, verbatim) and `eng_depth_at` (live qty at one level, tolerating a missing level). | The harness queries need values; upstream exposed them only through a `println!`-ing `get_bbo()` and otherwise-private fields. The raw BBO accessors deliberately return the engine's own cached fields so a staleness bug in `update_bbo` is *observed* by the audit, not masked by the adapter. |
| P4 | `cancel_order` returns `Result<(side, price, qty), ()>` (the resting price is threaded into the `order_loc` index); was `Result<&str, &str>`. | The harness CancelAck carries `side, order_id, price`. These are values the engine already holds in `order_loc` — it just discarded them. A read-path API enhancement, not a logic change. |
| **P5** | **Engine bug fix.** `match_at_price_level` now `break`s once `*incoming_order_qty == 0`. | Upstream iterates *every* maker in a price level even after the taker is exhausted; the trailing makers take the partial-fill `else` arm with `incoming_order_qty == 0` and execute `o.qty -= 0` / `done_qty += 0` — a no-op for the per-level qty sum upstream computed, but a **spurious zero-quantity fill** once per-maker fills are recorded (and wasted work regardless). Quantity non-conservation at the per-fill level. |
| **P6** | **Engine bug fix.** (a) `update_bbo` resets `best_bid_price` / `best_offer_price` to the empty sentinels (`u64::MIN` / `u64::MAX`) before scanning; (b) `cancel_order` calls `update_bbo` after a successful removal. | (B1) Upstream never reset the cached BBO before its scan, so when a side empties completely the loop finds no level and the **stale prior best survives** — a `best_bid`/`best_ask` query then returns a price no longer in the book. (B2) Upstream never refreshed the BBO on cancel at all, so a cancel that empties the current best level leaves the cached BBO **stale until the next add**. Query-correctness bugs. |

These are engine-source fixes, not adapter workarounds: the adapter relies on the
engine actually holding every order (it uses the engine's `order_loc` index as
its reject oracle) and on the engine's own BBO/depth being correct (the audit
reads them straight back). Fixing in the shim would mask, not resolve, the
engine's behaviour.

**Why these are load-bearing.** On the canonical `normal`/seed-23 tape, the
patched engine is VALID (hash PASS, 62,474 trades, 192/192 state checks). With P5
and P6 removed (P1–P4 kept so the adapter still compiles), the same engine is
INVALID: the spurious zero-quantity fills inflate the trade count to 184,683 (the
report hash diverges) and the stale BBO breaks 27 of 192 state checks. P5 was
originally observed as 12 zero-quantity fills on the `static` tape and P6 as 17–18
of 192 state-audit mismatches on the moving scenarios; after the fixes the
`static` report stream is byte-identical to the `liquibook` baseline.

No part of this is a performance route-around: the adapter uses the engine's
native id / cancel API, rebuilds no book state of its own, exports no
`engine_prebuild`, and pushes the engine's own fills through the transport.

## Build / run

```bash
bash additional_references/techieboy_adapter/build.sh
./harness --engine techieboy_adapter.so --scenario normal --mode audit \
          --matcher-core 72 --drainer-core 73
```

`build.sh` clones the engine into `third_party/techieboy_rust_orderbook/` at the
pinned commit, hard-resets it to that SHA, applies `patch_engine.py`, then builds
the engine + this adapter into `techieboy_adapter.so` at the repository root. Only
the engine crate is compiled (its sole dependency is `rand`, which the patch
leaves declared but no longer calls). The wrapper manifest declares its own empty
`[workspace]` so no enclosing workspace is pulled in, and the wrapper installs a
stable Rust toolchain into `$HOME/.cargo` only if `cargo` is not already on
`PATH`. Override the checkout with `ME_TECHIEBOY_SRC=/path/to/existing/clone` (the
engine repo root, i.e. the dir that contains `src/lib.rs`); the override checkout
is hard-reset to the pin and re-patched on every run, so an already-patched or
dirty override is restored first.
