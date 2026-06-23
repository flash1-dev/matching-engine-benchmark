# philipgreat_adapter — integration example

Wraps [philipgreat/lighting-match-engine-core](https://github.com/philipgreat/lighting-match-engine-core)
behind `api/matching_engine_api.h`.

Pinned commit: `381aeda4298524758db37d90c9a69f0fa5c8ca6c`.
License: MIT (`LICENSE.md`, "Copyright (c) 2025 PhilipGreat").

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot.

## Advertised vs measured

The project advertises roughly **125 M orders/s ("8 ns/order")**. That figure is
its `src/main.rs` micro-benchmark: a pre-seeded narrow book matched by a stream
of orders all at the **same two prices** (buy @ 10000 vs sell @ 1), which hits
the engine's flat-array "dense" book on a single hot cache line.

Driven by the harness's realistic order flow (a GBM mid-price, power-law
depth with a standing book, and a full cancel/modify/IOC lifecycle), the
same engine measures **4.43 M msgs/s** on `normal` with this adapter (clean
cores, 10-trial median), and `static` — whose ~21,000-order standing book
concentrates on a handful of sparse-map levels the engine scans linearly —
exceeds the 60 s per-trial budget outright. The collapse is expected and is
the point of the measurement — see "Which order book" below for why the
advertised dense path is unusable here.

## Engine shape

Pure Rust, single-threaded matcher. Two order books behind a common trait:

- `DenseOrderBook` — the advertised "8 ns" path. A flat `Vec<OrdersBucket>`
  indexed by `(price - base_price) / tick`. Every price must fall inside a
  fixed `[base_price, base_price + tick*(levels-1)]` window **and** be on-tick,
  or `match_order` returns `Err(PriceOutOfRange | PriceNotOnTick)` and drops the
  order. O(1) per level; the per-level bucket is a `VecDeque` (front = time
  priority).
- `SparseOrderBook` — a `BTreeMap<price, OrdersBucket>` per side, with **no**
  range/tick validation. Handles arbitrary `u64` prices; O(log levels).

Used natively by the adapter (sparse book):

- `SparseOrderBook::new(tick, base_price, max_levels, trade_cap)`.
- `match_order(OrderRequest) -> Result<(), OrderSubmitError>` — matches the
  incoming order, rests any limit residual, and leaves the resulting fills in
  `self.last_outcome.trades: Vec<Trade>` (cleared at the start of each call).
  **Trades are surfaced through this owned collection, not a return value or a
  callback** — the adapter reads the vec immediately after the call. Sparse
  never returns `Err`.
- `cancel_order(id) -> bool` — `true` when the order was resting and removed.
- `Trade { product_id, buy_order_id, sell_order_id, price, quantity,
  involves_mock_order }`. `price` is the **maker's** (resting order's) price.
  The resting order is always the maker and the taker is the incoming order, so
  the adapter recovers maker/taker ids from the incoming order's side.
- `bids` / `asks` (`pub BTreeMap<u64, OrdersBucket>`) — read directly for
  best-bid/ask and exact-price depth queries.

There is one product/symbol; the harness's single book maps to a fixed
`product_id = 7` (as `main.rs` uses). The protocol enum (`src/protocol/codec.rs`)
has `ORDER_SUBMIT` / `ORDER_CANCEL` / `TRADE_BROADCAST` / … but **no modify
message**, so the adapter composes modify = cancel + reinsert (the harness
contract anyway).

Not provided natively: no OrderAck / CancelAck / ModifyAck / Reject reports in
the harness's wire format. These are synthesised above the engine.

## Which order book: why sparse, not the advertised dense one

The harness `normal` workload's prices span the **entire** `[0, ~4.34e11]` tick
range (the GBM mid drifts across the run; median ≈ 3.3e5 ticks, p90 ≈ 2.2e11).
The dense book would need a `Vec` of ~4.34e11 `OrdersBucket`s — many terabytes —
so it is **infeasible** for this order flow. The adapter therefore drives the
**sparse** `BTreeMap` book, the only one of the two that can represent the
workload. (Prices are non-negative — the workload min is 0 — so the harness
`i64` ticks cast losslessly to the engine's `u64` price field.) The advertised
throughput is a dense-book single-price number and does not transfer to the
sparse path, which is why the measured figure is far lower.

## Adapter strategy

The upstream is pure Rust, so the adapter is one Rust `cdylib` exporting the
harness `engine_*` extern-C symbols directly — no C++ shim. Mirrors the
`orderbookrs_adapter` Rust pattern:

- **OrderAck first**, then a Trade per fill (read out of `last_outcome.trades`
  and stamped with the incoming order's seq), matching the canonical report
  order. Each Trade carries `price = trade.price` (maker's resting price),
  `maker_order_id`, `taker_order_id`, `quantity`.
- **IOC** is submitted as a normal limit; if the engine rests a residual the
  adapter `cancel_order`s it and emits one `CancelAck` for the unfilled
  quantity. Keeps emission order deterministic (Trades, then the residual
  CancelAck).
- **Modify** is the harness contract — cancel + reinsert (queue priority lost):
  cancel through the engine, emit exactly one `ModifyAck` at the new price/qty,
  then re-submit on the same side so any crossing fills emit as Trades stamped
  with the modify's seq.
- The engine's own id index — its `pub order_map: AHashMap<u64, (is_buy,
  price)>`, the same index its `cancel_order` consults — is the adapter's
  reject gate and the source of the side/price echoed on acks (with source
  patch 3 below, the index holds exactly the resting set). The adapter keeps
  exactly one per-order datum of its own: `AHashMap<u64, u32>` of
  `oid -> remaining` — the engine's tracking carries no quantity —
  maintained from the trades each match produces (partial fills decrement
  the maker, full fills remove the entry). It hashes with `ahash`, the
  engine's own hasher for the same u64 keys.
- Audit queries read the engine's `bids`/`asks` BTreeMaps directly, skipping
  levels whose orders are all cancelled (the engine prunes lazily): best-bid =
  highest bid key with a live order, best-ask = lowest ask key with a live
  order, depth = sum of active `remaining_quantity` in the requested price
  bucket.

## Source patches (applied by `build.sh`, idempotent)

The upstream is missing three corrections the harness's full lifecycle exposes.
All are applied with Python string replacements after `git reset --hard <sha>`,
are guarded so a re-apply is a no-op, and change only the three correctness
sites below — no other engine logic.

1. **`sparse.rs` — front-prune inside the inner match loop.** The sparse book
   prunes cancelled/depleted orders only at a bucket's front, *once*, before its
   inner matching `while`. Inside the loop it pops fully-filled orders but never
   re-prunes, so after the order ahead of it is consumed a cancelled order with
   `remaining_quantity == 0` left at the front is "matched" for
   `min(taker, 0) == 0` units, emitting a **phantom zero-quantity Trade**.
   Un-patched on the canonical `normal`, the engine emits 10,889 of them
   (73,363 Trade reports vs the 62,474-report consensus; first at seq 1,499:
   `1,1499,34205,0,503555,…`, a 0-share print against cancelled order
   503555). The dense book is immune because it prunes the front on every
   iteration; the patch adds the same `prune_bucket_front` to the sparse
   inner loop.

2. **`sparse.rs` + `dense.rs` — `cancel_order` must target the *active*
   instance.** Both books soft-delete on cancel (zero the quantity + set
   `is_cancelled`, prune only at the front), so a price bucket can briefly hold
   a dead tombstone **and** a live order with the same id — which the harness's
   modify == cancel + reinsert rule produces whenever a modify keeps the same
   price (a pure quantity increase): the modify cancels the original (leaving a
   non-front tombstone) and reinserts under the same id. A later real cancel
   then runs `find(|o| o.order_id == id)`, matches the **tombstone first**, and
   zeroes it again — leaving the live reinserted order resting forever and
   still matching. The patch changes the predicate to
   `o.order_id == id && o.is_active()` so a cancel hits the live order, not
   the stale duplicate.

3. **`sparse.rs` + `dense.rs` — `prune_bucket_front` must not disown a
   reinserted same-id order.** The lazy front-prune loop also removed each
   popped id from `order_map`, the engine's id index. Every order popped
   there was *already* removed from `order_map` at the moment it went
   inactive (`cancel_order` and the match loops disown immediately), so that
   remove was a no-op — except when a live order had since been reinserted
   under the same id (cancel + re-add, the standard modify idiom): there it
   deleted the **live** order's index entry, leaving it resting but
   unanswerable by id — a cancel of it spuriously fails while its liquidity
   keeps matching. The patch drops the remove so `order_map` holds exactly
   the resting set (patched in both books for parity). This matters doubly
   for this adapter, which reads `order_map` as its reject gate (above).

With the three patches the adapter's whole report stream is **byte-identical** to the
three-baseline consensus on `normal` (perf hash PASS) and the anti-cheat state
audit passes (192/192 probes match `liquibook`).

The upstream crate is also **binary-only** (`src/main.rs`, no `src/lib.rs`), so
`build.sh` additionally writes a re-export-only `src/lib.rs` that exposes the
engine modules, letting the wrapper crate link it as a path dependency. This is
mechanical (no logic change) and likewise idempotent.

## Build / run

```bash
bash additional_references/philipgreat_adapter/build.sh
./harness --engine ./philipgreat_adapter.so --scenario normal --mode perf  \
          --matcher-core 82 --drainer-core 83
./harness --engine ./philipgreat_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh` installs the stable Rust toolchain into `$HOME/.cargo` if `cargo` is
not on PATH (no sudo), clones the engine into
`third_party/lighting-match-engine-core/` at the pinned commit, `git reset
--hard`s it, applies the `src/lib.rs` shim and the three source patches above, and
builds the wrapper `cdylib` with `cargo build --release` and
`RUSTFLAGS="-C target-cpu=native"` (nothing more — the upstream ships no
effective `[profile.release]`, so per the house rule the adapter adds none).
Use `ME_PHILIPGREAT_SRC=/path/to/checkout` to build against an existing clone
(the script rewrites the wrapper crate's `Cargo.toml` path and restores it on
exit).
