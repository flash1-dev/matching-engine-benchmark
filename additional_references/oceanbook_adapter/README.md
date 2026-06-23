# oceanbook_adapter — integration example

Wraps [draveness/oceanbook](https://github.com/draveness/oceanbook) behind
`api/matching_engine_api.h`. oceanbook is a pure-Go price-time-priority
limit-order book (the order book that backs the *draven/oceanbook* trading-system
write-up).

Pinned commit: `a7768eed53a239faf883144090fd48931129f145` (repo HEAD).

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Single `OrderBook` (`pkg/orderbook`), a single-threaded matcher guarded by the
book's own `sync.RWMutex`. Native APIs visible from the adapter:

- `OrderBook.InsertOrder(*order.Order) []*trade.Trade` — the only matching entry
  point: crosses the incoming order against the opposite book and returns one
  `*trade.Trade` per fill. Each `trade.Trade` carries the **maker's** resting
  price and the maker/taker ids — exactly the harness `Trade` fields (fills
  cross at the maker price; `order.Match` prices the trade from `maker.Price`).
- `OrderBook.CancelOrder(*order.Order)` — removes a resting order **by id** (it
  looks the id up in the engine's own `cancelOrdersQueue` and removes it from
  the correct `Bids`/`Asks` tree). It silently no-ops on a missing id and
  returns nothing.
- `Bids` / `Asks : *redblacktree.Tree` (emirpasic/gods) — the authoritative
  resting book, keyed by `order.Comparator` over `(price, CreatedAt, id)`. The
  tree's right-most node is the best price (the match loop crosses against
  `makerBooks.Right()`).
- `order.Order{ID, Side, Price, Quantity, CreatedAt, ImmediateOrCancel}` with
  `shopspring/decimal` prices/quantities; price-tie priority is by `CreatedAt`
  then id.

Not provided natively: no per-order accept/ack or reject events (only the trade
slice); no in-place modify (cancel + reinsert); no IOC residual notification (an
`ImmediateOrCancel` order's residual is discarded, never inserted); no public
numeric best-bid/ask/depth accessor (`Bids`/`Asks` are exported but the price
lives in the unexported node payload, and the only depth surface,
`Depth.Serialize`, returns protobuf strings). `uint64` order ids map straight to
`order.Order.ID` (no string keys).

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `oceanbook_adapter.so` with plain C
  symbols the harness's `dlopen` resolves directly. No C++ shim. oceanbook is
  importable (its `pkg/*` are `package orderbook` / `order` / `trade`), so the
  wrapper imports it directly through a `replace` directive `build.sh` rewrites
  to the pinned `third_party` checkout.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one `InsertOrder` /
  `CancelOrder` per crossing. Without `engine_on_batch` the run would measure
  the per-call cgo boundary into the Go runtime, not the matcher; each message
  is still dispatched in array order with no lookahead. See
  `docs/METHODOLOGY.md` "Batch delivery".
- For each new order: emit `OrderAck`, `InsertOrder` a GTC (or IOC when
  `o.ioc`) `order.Order`, then emit one `Trade` per returned `trade.Trade`
  (maker price, aggressor seq, maker/taker ids), in match order.
- **IOC**: delegated via `order.Order.ImmediateOrCancel` (the engine never rests
  the residual); the adapter emits the harness `CancelAck` for `qty − Σ fills`.
- **Modify**: cancel + reinsert (the engine has no native modify).
  `CancelOrder(&order.Order{ID: oid})` (native id-based removal), then
  `InsertOrder` a fresh GTC order at the new price/qty; the reinsert's crossing
  fills carry the modify's seq. One `ModifyAck`, or `ModifyReject` if the order
  is not resting.
- **Time priority**: oceanbook breaks a price tie by `CreatedAt` then id, so the
  adapter stamps each inserted order with a strictly increasing `CreatedAt`,
  giving true FIFO arrival order — matching the `liquibook` baseline.
- **Liveness / reject adjudication + side·price echo** come from a small shadow
  `map[id]{price,side,remaining,alive}` (oceanbook returns no per-order
  accept/reject and `CancelOrder` silently no-ops on a missing id, so the
  adapter must decide whether a cancel/modify targets a live order). The shadow
  is allocated once in `engine_init`, kept consistent by replaying the returned
  trades (each maker fill decrements that maker's remainder; a maker that reaches
  zero is retired, mirroring the engine), and is used **only** for reject
  adjudication and the ack side/price echo — **not** for the audit queries.
- **Audit queries** read the engine's **authoritative resting book** — the
  `Bids`/`Asks` order trees — via the read-only `HarnessBestBid` /
  `HarnessBestAsk` / `HarnessDepthAt` accessors `build.sh` adds (see *Source
  patch*), not from the shadow. Best-bid/ask return the order tree's right-most
  node (the exact node the match loop crosses against); depth-at-price sums the
  remaining quantity of the live resting orders at that price. So the state
  audit sees the engine's real resting state, including the effect of cancels.
- Decimals: workload prices/quantities are positive integers carried as
  `decimal.New(n, 0)` (the exact integer decimal), so engine comparisons
  preserve tick order bit-for-bit and `IntPart()` recovers the int64 exactly.

## Source patch

`build.sh` applies **one engine correctness fix** and **adds one read-only query
file**. Both are applied to a pristine checkout (post `git reset --hard`),
loud-fail if their anchors move, and are idempotent (a marker guard on the
override path; the reset restores pristine source on the default checkout).

**1. Depth price-level accounting (correctness fix).** oceanbook's market-data
`Depth` writes the trade/order **quantity into the `PriceLevel.Price` field**
and leaves `PriceLevel.Quantity` (and `Count`) at zero. `OrderBook.insertOrder`
(`pkg/orderbook/orderbook.go`) updates `Depth` in two places, both passing the
quantity as the price:

```go
// on each fill (maker side):
od.depth.UpdatePriceLevel(&PriceLevel{Side: bestOrder.Side, Price: newTrade.Quantity.Neg()})
// when the taker's residual rests:
od.depth.UpdatePriceLevel(&PriceLevel{Side: newOrder.Side, Price: newOrder.PendingQuantity()})
```

So every depth level reports `price = ±qty`, `quantity = 0`, `count = 0`, and a
level never prunes by count. The **match path is unaffected** — matching
iterates the `Bids`/`Asks` order trees and each order's remaining quantity, never
the `Depth` struct — so the trade/report stream is byte-identical to the
`liquibook` baseline on all five scenarios (the report hash matches); the
corruption surfaces only through `Depth` (the engine's gRPC market-data
surface). Classification: hard-invariant violation in the engine's published
depth surface (off the match path).

The fix puts the real price in `Price`, the quantity in `Quantity`, and
maintains `Count` (`+1` when a new level rests; `-1` when a maker is fully filled
and leaves the book, so the level prunes correctly). Two edits, no behaviour
change beyond the `Depth` accounting, so every report hash still matches the
baseline. Filed upstream as
[draveness/oceanbook#44](https://github.com/draveness/oceanbook/issues/44).

Scope note: this fix corrects the engine's own `Depth` market-data surface. The
adapter's state audit reads the **authoritative `Bids`/`Asks` order trees** (fix
#2 below), not `Depth`, because `Depth` is also never updated by `CancelOrder`
(an engine quirk independent of #44) and so would go stale after a cancel even
once patched. Because the #44 bug lives entirely in that off-the-match-path
`Depth` aggregate, the harness `normal`/seed-23 verdict is **VALID** with the fix
applied (matching is byte-identical and the audit reads the order trees); the
fix is shipped so the third_party engine is the corrected, conforming engine the
project records for oceanbook.

**2. Read-only audit accessors (`harness_query.go`).** oceanbook exposes no
numeric best-bid/ask/depth accessor the audit needs. `build.sh` drops in **one
new file**, `pkg/orderbook/harness_query.go` (same `package orderbook`, so it
reads the unexported node payload), modifying no existing source. It exports
three read-only, side-effect-free accessors used by `engine_query_*`, all
reading the authoritative `Bids`/`Asks` order trees:

- `HarnessBestBid() (int64, bool)` / `HarnessBestAsk() (int64, bool)` — the
  order tree's right-most node price (the node the match loop crosses against).
- `HarnessDepthAt(scaledPrice int64, side order.Side) int64` — the sum of the
  live resting orders' remaining quantity at the price.

No matching logic is touched; the accessors read the live engine book exactly as
the engine maintains it, so the engine's real resting state (cancels included)
is surfaced to the state audit rather than hidden behind an adapter shadow.

## Build / run

```bash
bash additional_references/oceanbook_adapter/build.sh
./harness --engine oceanbook_adapter.so --scenario normal --mode audit \
          --matcher-core 64 --drainer-core 65
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the
   right tarball.
2. Clones the engine into `third_party/oceanbook/` at the pinned commit,
   `git reset --hard`s to it, applies the Depth accounting fix, and drops in
   `harness_query.go`.
3. Rewrites the wrapper's `replace` directive to the checkout (relative path)
   and builds the cgo wrapper under `wrapper/` with
   `go build -buildmode=c-shared`, writing `oceanbook_adapter.so` at the harness
   repo root.

Override the upstream checkout: `ME_OCEANBOOK_SRC=/path/to/checkout` skips the
clone (the source patch and query file are re-applied in place, idempotently).
