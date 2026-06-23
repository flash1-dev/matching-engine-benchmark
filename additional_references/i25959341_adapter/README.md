# i25959341_adapter — integration example

Wraps [i25959341/orderbook](https://github.com/i25959341/orderbook) behind
`api/matching_engine_api.h`. i25959341/orderbook is a pure-Go price-time-priority
limit-order book ("Improved matching engine written in Go (Golang)", ~550★, self-
described "above 300k trades per second").

Pinned commit: `0d883ab1157580d58ba9f2b9c537a3363310231c` (`add go mod and fix
tests (#19)`, repo HEAD).

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

Single `OrderBook` over two `*OrderSide` (`bids` / `asks`), a single-threaded
matcher. Each `OrderSide` is an [emirpasic/gods](https://github.com/emirpasic/gods)
red-black tree of price levels keyed by `decimal.Decimal`, plus a
`map[string]*OrderQueue` price index; each `OrderQueue` is a `container/list`
FIFO of `*Order` (time priority). `OrderBook.orders : map[string]*list.Element`
is the id (string) → order lookup. Native APIs visible from the adapter:

- `ProcessLimitOrder(side, id, qty, price) (done []*Order, partial *Order,
  partialQuantityProcessed decimal.Decimal, err error)` — the only matching entry
  point; it **returns** the match result (no per-fill callback). Every
  fully-consumed maker is in `done`, in match order, each carrying its own
  resting price and the full consumed quantity; the single partially-consumed
  maker (if any) is in `partial` with the amount taken in
  `partialQuantityProcessed`; and when the taker fully fills, the engine appends
  a synthetic average-price order carrying the **taker's** id as the last element
  of `done`. Fills cross at the **maker's** resting price.
- `CancelOrder(id) *Order` — removes a resting order, returns the removed
  `*Order`, or `nil` if no such id is resting (never seen / already filled /
  already cancelled). This is the engine's own reject adjudication.
- `GetOrderSide(side) *OrderSide`, and on a side `MaxPriceQueue()` /
  `MinPriceQueue()` / `LessThan(price)` (tree navigation) and on a queue
  `Price()` / `Volume()` (the price level's own aggregate) — read-only.

Not provided natively: no IOC / FOK order type; no in-place modify; `string`
order ids (the harness uses `uint64_t`); the `ProcessLimitOrder` / `CancelOrder`
results carry no ready-made wire report with the resting order's side, and there
is no query that tells a resting id from a filled/cancelled one. `Side` is
`Sell = 0, Buy = 1` internally; prices and quantities are `shopspring/decimal`.

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `i25959341_adapter.so` with plain C
  symbols the harness's `dlopen` resolves directly. No C++ shim. The engine is
  importable (`package orderbook`, module `orderbook`), so the wrapper imports it
  directly through a `replace` directive `build.sh` rewrites to the pinned
  `third_party` checkout.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one `ProcessLimitOrder`
  / `CancelOrder` per crossing. Without `engine_on_batch` the run measures the
  per-call cgo boundary into the Go runtime, not the matcher; each message is
  still dispatched in array order with no lookahead. See
  `docs/METHODOLOGY.md` "Batch delivery".
- For each new order: emit `OrderAck`, `ProcessLimitOrder` (GTC), then emit one
  `Trade` per real maker fill (maker price, aggressor seq, maker/taker ids), in
  match order. The wrapper drops the trailing synthetic taker element of `done`
  (it is the average-price aggressor summary, not a maker fill) and reads the
  partially-consumed maker out of `partial` + `partialQuantityProcessed`.
- **IOC**: the engine has no IOC type, so the adapter `ProcessLimitOrder`s the
  order as a normal limit and then `CancelOrder`s any resting residual,
  emitting the harness IOC-residual `CancelAck` for `qty − Σ fills`.
- **Modify**: cancel + reinsert (the engine has no native modify).
  `CancelOrder(id)`, then `ProcessLimitOrder` a fresh order at the new
  price/qty; the reinsert's crossing fills carry the modify's seq. One
  `ModifyAck`, or `ModifyReject` if the order is not resting.
- **Liveness / reject adjudication** come from the engine itself: `CancelOrder`
  returns `nil` for a non-resting id, which the adapter maps to
  `CancelReject` / `ModifyReject`. A small shadow
  `map[id]{price,side,remaining,alive}` supplies only the side/price **echo** for
  the cancel/modify reports (the engine's result doesn't carry a ready-made
  report); it is allocated once in `engine_init` and is **not** consulted for the
  audit queries.
- **Audit queries** read the **live engine book**: `engine_query_best_bid` /
  `engine_query_best_ask` walk the side's price tree
  (`GetOrderSide(...).MaxPriceQueue()` / `MinPriceQueue()`), and
  `engine_query_depth_at` reads the matching price level's own `OrderQueue.Volume()`
  — the per-price-level aggregate the engine maintains — so the engine's real
  internal depth accounting is what the state audit sees, not an adapter shadow.
- Decimals: workload ticks/quantities are positive integers; `decimal.New(v, 0)`
  builds an integer-valued decimal whose `IntPart()` recovers `v` exactly, so the
  int64 ↔ decimal round-trip is lossless and the tree's `Cmp` ordering matches
  integer ordering bit-for-bit.
- Order id: `uint64 → strconv.FormatUint(.,10)` — the engine's native
  string-keyed `map[string]*Order` design; the allocation is intrinsic to the
  engine, not adapter-added overhead.

## Source patch

`build.sh` applies **one engine correctness fix** (per-side resting-volume
conservation). It is applied to a pristine checkout (post `git reset --hard`),
loud-fails if its anchors move, and is idempotent (a marker guard on the
override path; the reset restores pristine source on the default checkout). No
files are added to the engine package — the audit reads the engine's own
public accessors.

**Per-side aggregate volume after a partial fill (correctness fix).**
`OrderSide.volume` is the per-side total resting quantity. It is incremented in
`OrderSide.Append` (`orderside.go:67`) and decremented in `OrderSide.Remove`
(`orderside.go:86`) — but a **partial** fill of a resting order never goes
through `Remove`: `OrderBook.processQueue` (`orderbook.go:206-210`) shaves the
consumed quantity off the front order with `OrderQueue.Update()`, which keeps the
per-**price-level** `OrderQueue.volume` correct (`orderqueue.go:60-65`) but leaves
`OrderSide.volume` untouched. So after any partial fill `OrderSide.Volume()`
over-reports by the consumed quantity until that order is later fully removed.

Classification: hard-invariant violation in principle (per-side resting-depth
non-conservation), but **off the harness-read path**. The harness state audit's
depth check reads the **per-price-level** `OrderQueue.Volume()` (via the
adapter's `engine_query_depth_at`), which `Update()` maintains correctly — it
never reads the per-side `OrderSide.Volume()` aggregate. The trade/report stream
is likewise unaffected (matching iterates `OrderQueue.Len()` / `Head()`, never
reads `volume`). **This engine is therefore already `VALID` on the harness
without the patch** (report hash byte-identical to the `liquibook` baseline and
all 192 state checks pass, on every scenario); the patch corrects the *other*
reader, `OrderSide.Volume()`, for completeness. The finding was reported upstream
as a duplicate of an already-open report of the same per-side accounting issue.

Fix: thread the consumed `*OrderSide` into the queue-processing routine (a new
`processQueueSide`; `processQueue` becomes a thin `nil`-side wrapper, preserving
the public signature) and subtract the consumed quantity from `OrderSide.volume`
at the partial-fill site, alongside the existing `OrderQueue.Update`. Both
matching call sites (`ProcessLimitOrder` / `ProcessMarketOrder`) already hold the
side being consumed and are routed through the new entry point. No behaviour
change beyond the per-side aggregate, so every report hash and the 192 state
checks still match the baseline (VALID ×5 across 100 seeds, patched and
unpatched alike).

## Build / run

```bash
bash additional_references/i25959341_adapter/build.sh
./harness --engine i25959341_adapter.so --scenario normal --mode audit \
          --matcher-core 54 --drainer-core 55
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the
   right tarball.
2. Clones the engine into `third_party/i25959341_src/` at the pinned commit,
   `git reset --hard`s to it, and applies the per-side volume fix.
3. Rewrites the wrapper's `replace orderbook =>` directive to the checkout
   (relative path) and builds the cgo wrapper under `wrapper/` with
   `go build -buildmode=c-shared`, writing `i25959341_adapter.so` at the harness
   repo root.

Override the upstream checkout: `ME_I25959341_SRC=/path/to/checkout` skips the
clone (the source patch is re-applied in place, idempotently).
