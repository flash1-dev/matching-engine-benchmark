# danielgatis_adapter — integration example

Wraps [danielgatis/go-orderbook](https://github.com/danielgatis/go-orderbook)
behind `api/matching_engine_api.h`. danielgatis/go-orderbook is a pure-Go
price-time-priority limit-order book (the WK Selph design): a red-black tree of
price levels (`emirpasic/gods`) whose leaves are FIFO `container/list` queues,
an id→`*list.Element` map for cancel, and `shopspring/decimal` prices and
quantities.

Pinned commit: `7640955559eb5473c36a56507d3eadf830c66713` ("update deps", repo
HEAD).

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Single `OrderBook` (`NewOrderBook(symbol)`), a single-threaded matcher. Native
APIs visible from the adapter:

- `ProcessLimitOrder(orderID, traderID, side, amount, price) ([]*Trade, error)`
  — matches the incoming order against the resting book and **rests the
  residual**; returns the fills as a `[]*Trade` (`MakerOrderID`, `Amount`,
  maker `Price`), in match order. No callback.
- `CancelOrder(orderID) *Order` — removes a resting order; returns the removed
  `*Order`, or `nil` if no such order is resting (the reject signal).
- Order ids and trader ids are **strings**; prices and quantities are
  `shopspring/decimal.Decimal`.

Not provided natively: no IOC / FOK / POST-ONLY mode (submit a normal limit then
cancel the unfilled remainder); no in-place modify — the engine's own README
says *"Updates will have to be handled with Cancel+Create"*; the engine has
self-trade prevention (`ProcessLimitOrder` skips a resting maker whose
`traderID` equals the aggressor's, `order_book_limit.go:71`).

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `danielgatis_adapter.so` with plain
  C symbols the harness's `dlopen` resolves directly. No C++ shim.
  danielgatis/go-orderbook is importable (`package orderbook`), so the wrapper
  imports it directly through a `replace` directive `build.sh` rewrites to the
  pinned `third_party` checkout.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one `ProcessLimitOrder`
  / `CancelOrder` per crossing (without it the run measures the per-call cgo
  boundary into the Go runtime, not the matcher). Each message is still
  dispatched in array order with no lookahead. See `docs/METHODOLOGY.md`
  "Batch delivery".
- For each new order: emit `OrderAck`, `ProcessLimitOrder` (the engine matches
  and rests the residual), then emit one `Trade` per returned fill (maker price,
  aggressor seq, maker/taker ids), in match order.
- **IOC**: the engine has no IOC mode, so the adapter submits a normal limit and
  then `CancelOrder`s the rested residual, emitting the harness `CancelAck` for
  `qty − Σ fills`.
- **Modify**: cancel + reinsert (the engine has no native modify, per its own
  README). `CancelOrder(id)`, then `ProcessLimitOrder` a fresh order at the new
  price/qty (the reinsert may itself cross and produce trades, which carry the
  modify's seq). One `ModifyAck`, or `ModifyReject` if the order is not resting.
- **Reject adjudication** is the engine's: `CancelOrder` returns the removed
  `*Order` or `nil`, so a cancel/modify of an id that is not resting (never
  seen, already filled, already cancelled) rejects on the engine's own `nil`
  return — no adapter-side pre-check, the engine's id map is the source of
  truth. The removed `*Order`'s side and price are echoed on the `CancelAck`.
- **Order-id / trader-id mapping**: the harness `uint64` id →
  `strconv.FormatUint(.,10)` (the engine's native API is string-keyed; the
  string is intrinsic to driving it, not adapter-added overhead). The trader id
  is set to the order-id string, so every order is from a **distinct** trader —
  this makes the engine's self-trade prevention inert for cross-order matching
  (an order can never match itself), which is the faithful single-book
  price-time mapping.
- **Prices / quantities**: int64 ticks → `decimal.NewFromInt(ticks)`, recovered
  with `.IntPart()`; `uint32` qty → `decimal.NewFromInt(int64(qty))`. Integer
  ticks round-trip exactly.
- **Audit queries** (`engine_query_best_bid` / `_best_ask` / `_depth_at`) are
  answered from a small shadow `map[id]{price,side,remaining,alive}` rather than
  the engine: the engine exposes `Depth()` only as an O(book) scan and no cheap
  best-bid/ask peek. The shadow mirrors the resting book (decremented as the
  engine reports fills, retired on cancel/full-fill), is allocated **once** in
  `engine_init`, and is read only by the rare audit queries — never on the hot
  path. The state audit passes (192/192) against the engine's real fills,
  because the shadow is driven by the engine's own returned `[]*Trade`.

There are no hot-path heap allocations the engine itself doesn't impose (a
single package-level report struct is reused for every emission; the shadow map
is pre-sized once) and no locks (single matcher thread). No `engine_prebuild`
is exported.

## Source patch

`build.sh` applies **one engine correctness fix**. It is applied to a pristine
checkout (post `git reset --hard`), loud-fails if its anchors move, and is
idempotent (a marker guard on the `ME_DANIELGATIS_SRC` override path; the reset
restores pristine source on the default checkout).

**Same-price orders must share one price level (correctness fix).**
`OrderSide` keys its price levels by a `map[decimal.Decimal]*OrderQueue`
(`order_side.go:16`). `shopspring/decimal.Decimal` is a struct
`{value *big.Int; exp int32}`, and a Go map compares struct keys field-by-field
— for the `*big.Int` field that is a **pointer** comparison, not a numeric one.
Two equal prices built independently (e.g. `decimal.NewFromInt(100)` for two
different orders) hold different `*big.Int` pointers, so they are **distinct**
map keys even though `decimal.Cmp == 0`:

```go
m := map[decimal.Decimal]...{}
m[decimal.NewFromInt(100)] = x
_, ok := m[decimal.NewFromInt(100)]   // ok == false  (pointer mismatch)
```

`Append` (`order_side.go:37`) therefore misses the existing queue for a price
that is already resting, creates a **second** `OrderQueue`, and
`os.tree.Put(price, ...)` — whose comparator *is* numeric — overwrites the tree
node for that price with the new queue, **orphaning every order already resting
in the first queue**. The orphaned orders vanish from the book: they never match
and a later same-price crossing finds nothing. (`static`, which only ever rests
one order per price, passes; the moving scenarios under-match — ~61%.)

Classification: hard-invariant violation (resting orders silently dropped). The
fix keys the price-level map by the price's canonical decimal **string**
(`decimal.String()`), which is identical for equal prices, so they share one
`OrderQueue`. Seven edits, all in `order_side.go` — the map's field type, its
allocation, and the five places it is indexed by the raw decimal price; the
red-black tree (already correctly numeric via `.Cmp`) and all matching/queue
logic are untouched. With the fix the report-stream hash matches the `liquibook`
baseline and the state audit passes (VALID ×5 across 100 seeds). Filed upstream
as
[danielgatis/go-orderbook#2](https://github.com/danielgatis/go-orderbook/issues/2).

A read-only reproduction of the underlying `map[decimal.Decimal]` semantics
(independent of the harness) is in the upstream issue; on the unpatched engine
the harness reports `Verdict: INVALID` (hash FAIL, 138/192 state checks
mismatched, 58 207 vs 62 474 trades on `normal`).

## Build / run

```bash
bash additional_references/danielgatis_adapter/build.sh
./harness --engine danielgatis_adapter.so --scenario normal --mode audit \
          --matcher-core 58 --drainer-core 59
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the
   right tarball.
2. Clones the engine into `third_party/danielgatis_src/` at the pinned commit,
   `git reset --hard`s to it, and applies the price-key fix.
3. Rewrites the wrapper's `replace` directive to the checkout (relative path)
   and builds the cgo wrapper under `wrapper/` with
   `go build -buildmode=c-shared`, writing `danielgatis_adapter.so` at the
   harness repo root.

Override the upstream checkout: `ME_DANIELGATIS_SRC=/path/to/checkout` skips the
clone (the source patch is re-applied in place, idempotently).
