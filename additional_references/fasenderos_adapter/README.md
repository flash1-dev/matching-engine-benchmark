# fasenderos_adapter — integration example

Wraps [fasenderos/nodejs-order-book](https://github.com/fasenderos/nodejs-order-book)
("nodejs-order-book") behind `api/matching_engine_api.h`. The engine is a
TypeScript/Node.js limit order book (price-time priority; per side a
`functional-red-black-tree` of price levels, each a `denque` FIFO of orders, plus
an id→order map), so there is no native engine library to link: the adapter
embeds the system **libnode**'s V8 in-process and drives the engine's `OrderBook`
through a thin JS shim.

Pinned commit:
- `fasenderos/nodejs-order-book` — `f8e285bd2179392abe358ecb02f0fd3b76486178`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot
(nodejs-order-book: no fix required).

## Engine shape

`OrderBook` class (`src/orderbook.ts`), single-threaded synchronous matcher.
Native APIs the shim uses:

- `OrderBook.limit({ type, id, side, size, price, timeInForce? })` — submits a
  limit order; crosses it against the opposite side at the **maker** (resting)
  price, then rests any residual. `timeInForce: IOC` makes it immediate-or-cancel
  natively (the residual is dropped, not rested).
- `OrderBook.cancel(id)` — native cancel-by-id; returns `undefined` (or an object
  whose `.order` is `undefined`) when the order is not resting.
- `OrderBook.modify(id, { price, size })` — native modify; the engine itself does
  a cancel + reinsert (so a price/size change loses time priority and may cross),
  and sets `err = ORDER_NOT_FOUND` when the order is not resting.
- `OrderBook.order(id)` — id→order lookup (authoritative "is this resting?").
- `OrderBook.depth()` — `[asks, bids]` price-level snapshots, each sorted
  best-first; used for the harness state-audit queries.

Order ids are strings natively; the harness uses `uint64_t`. Prices and sizes are
integers. Not exposed natively: no FOK / POST-ONLY; no explicit Ack/Reject events
(the adapter synthesises those from the call's return value); and **no per-fill
trade event** — fills are reported only inside a post-hoc `IProcessOrder` summary,
which is why the adapter injects a per-fill hook (see "Source patch").

## Adapter strategy

The C++ side (`fasenderos_adapter.cpp`) starts one V8 isolate in `engine_init`,
evaluates a self-contained JS bundle (the engine + its two pure-JS deps + the
entry shim `entry.js`), and caches handles to the flat `globalThis.LOB` API the
shim exposes. Every `engine_*` ABI call invokes the matching **synchronously on
the calling (matcher) thread** and turns the result + per-fill events into the
report stream. No Node event loop, no out-of-process server, no matcher worker.

- **Reports come from the engine.** A `ME_TRADE` is emitted for each fill the
  patched engine reports through the hook; `Trade.price_ticks` is the **maker's
  resting price** and `Trade.sequence_number` / `taker_order_id` are the
  aggressor's (threaded through a per-call context set just before the `limit()`
  call). Acks/rejects are derived from each call's native return value.
- **Boundary buffers, no per-message JS object churn.** The shim writes the
  per-message status into a fixed `Int32Array` and the per-fill `{maker, taker,
  price, qty}` into four grow-only `Float64Array`s; the C++ side caches raw
  pointers to those backing stores once at init and reads them directly after
  each call (it does not allocate JS objects on the boundary per message).
- **Prices**: the harness's signed `int64` ticks are passed straight through as JS
  numbers (integer-valued, exact in IEEE double over the workload range) and read
  back the same way; nodejs-order-book imposes no positive-price constraint, so no
  offset map is needed.
- **Order-id mapping**: the engine names orders with strings, so the shim does
  `String(id)` on the way in and `+makerId` on the way out. No adapter-side
  id→handle book is built — existence is answered by the engine's own
  `order(id)` / the `cancel`/`modify` return codes.
- **IOC**: submitted natively with `timeInForce: IOC`; the engine drops the
  residual, and the adapter emits one `CancelAck` for the unfilled remainder
  (computed from the taker's filled tally) so an IOC order never rests.
- **Modify**: the engine's **native** `modify` (which does cancel + reinsert) is
  called; its crossing fills carry the modify message's `sequence_number`, then a
  `ModifyAck` (or `ModifyReject` if the order was not resting).
- **Cancel / modify of a non-resting order** (already filled / cancelled / never
  seen) → `CancelReject` / `ModifyReject`, adjudicated by the engine's own
  `cancel` return / `modify` `err` — no adapter shadow book.

`engine_flush` is a no-op: matching and report emission complete synchronously
inside each `engine_on_*` call. The adapter exports **no** `engine_prebuild`, so
the harness runs no prebuild pass (nothing is inserted into the book ahead of the
timed run). The harness reports ~18 engine threads "after_init" with "+0"
after_run — these are libnode's own V8/runtime background threads, not matcher
workers; all matching runs on the harness's matcher thread inside the single
isolate.

## Source patch

`build.sh` applies **one** source patch to the engine, after `git reset --hard`
to the pin so the reset can never clobber it (idempotent — guarded by a marker so
a re-apply is a no-op; fails loud if either anchor drifts):

- **Per-fill trade hook** (adapter instrumentation, `src/orderbook.ts`).
  `OrderBook.processQueue` consumes one resting maker per loop iteration but
  surfaces fills only in a post-hoc `IProcessOrder` summary — it never emits a
  per-fill event carrying the maker's resting price + the fill quantity in match
  order, which is what the harness needs for one `ME_TRADE` per fill. The patch
  inserts one guarded global hook call,
  `(globalThis).__ME_onFill(makerId, makerPrice, qty)`, at each of `processQueue`'s
  two fill sites — the partial-fill branch (qty filled = the pre-zeroing
  `response.quantityLeft`) and the full-fill branch (qty filled =
  `headOrder.size`). It adds only those two emit points; the matching logic,
  prices and quantities are otherwise byte-identical to the pinned source, and the
  entry shim implements the hook. This is the same "the engine declares the event
  but never emits it" instrumentation pattern used by `jxm35_adapter` and
  `kautenja_adapter`.

This hook is **not** a correctness fix. nodejs-order-book is **conforming as
shipped** — it already crosses at the maker price, exposes native cancel/modify by
id, and answers existence authoritatively — so the harness classifies it "No fix
required" (`CORRECTNESS_FINDINGS.md`) and the state audit (192 checks) passes
against the unmodified matching/cancel/modify logic. No upstream issue is filed:
the adapter only adds an event emit point, it does not change any matcher
behaviour.

## Build / run

```bash
bash additional_references/fasenderos_adapter/build.sh
./harness --engine fasenderos_adapter.so --scenario normal --mode audit \
          --matcher-core 66 --drainer-core 67
```

`build.sh` clones the engine into `third_party/fasenderos_nodejs_order_book/` at
the pinned commit, applies the trade-hook patch, `npm install`s the engine's two
pure-JS deps (`denque`, `functional-red-black-tree`) plus `esbuild`, bundles
`entry.js` + engine + deps into one self-contained IIFE, embeds it as a C header,
and compiles `fasenderos_adapter.cpp` against the system libnode V8 embed headers
into `fasenderos_adapter.so` at the repo root. All build scaffolding (the staged
shim, `bundle.js`, `bundle_js.h`) lives under the gitignored `third_party/` tree,
so the committed adapter directory holds only authored files
(`fasenderos_adapter.cpp`, `entry.js`, `crypto_stub.js`, `build.sh`, `README.md`).

Requirements: the system **libnode** plus its development headers (`v8.h` +
`libplatform/libplatform.h` under `/usr/include/node`, e.g. `apt-get install
libnode-dev`) — the analogue of the `jlob` JVM and `pyme` CPython embeds; like
those it cannot be provisioned root-free from a tarball, so it must already be
present (`build.sh` checks and fails loud otherwise). `node` + `npm` are needed
only at **build** time (to run esbuild and fetch the two deps), not at run time.
Overrides: `ME_FASENDEROS_SRC=/path/to/existing/clone` uses an existing checkout
in place of cloning (the patch is re-applied idempotently); `ME_NODE_INC` points
at a different node header tree.
