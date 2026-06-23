# gotrader_adapter — integration example

Wraps [robaho/go-trader](https://github.com/robaho/go-trader) behind
`api/matching_engine_api.h`. go-trader is a FIX/gRPC exchange written in Go
(513★); its limit-order book (`internal/exchange`: `orderBook.add` / `.remove` /
`matchTrades`) is a separable in-process unit, and this adapter drives that book
directly — no FIX session, no gRPC, no QuickFIX runtime is stood up.

Pinned commit: `1d34bc8206d7931939e02142f582a0a009b1da3b`.

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

A single in-process `orderBook` per instrument, single-threaded matcher. The
matcher's surface — all UNEXPORTED, reachable only from within the engine module:

- `orderBook.add(sessionOrder) ([]trade, error)` — submits a limit order,
  crosses it against the resting book, and returns the fill list (go-trader
  returns a slice, not a callback). The residual of a non-IOC order rests.
- `orderBook.remove(sessionOrder)` — cancels by handle. `allOrders` is keyed by
  the full `sessionOrder` value, so a cancel must present the exact
  `sessionOrder` that was added.
- `matchTrades` prices each fill at the **resting** order's price (it picks the
  order with the earlier `sessionOrder.time` as the maker).
- `orderBook.bids` / `.asks` — `[]priceLevel`, best level first; each level is a
  FIFO singly-linked list of order nodes (time priority).
- Prices/quantities are `robaho/fixed.Fixed` (a `decimal` with 7 fraction
  digits stored as a single scaled `int64`).

Not provided natively in a form the harness can call directly: the matcher's
types are unexported (`orderBook`, `sessionOrder`, `add`, `remove` are all
lowercase), so an out-of-package `main` cannot reach them; there is no IOC flag
on `add` (the residual always rests); there is no numeric best-bid/ask/depth
reader; order ids are the engine's `OrderID`. go-trader DOES have a native
in-place `ModifyOrder` (it is internally cancel-then-reinsert).

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `gotrader_adapter.so` with plain C
  symbols the harness's `dlopen` resolves directly. No C++ shim.
- **In-module placement.** Because the matcher is unexported, the adapter's two
  Go files are added INSIDE the cloned engine module (the only way an in-process
  Go caller can drive an unexported matcher):
  - `internal/exchange/me_shim.go` — an exported shim (`package exchange`) over
    the unexported book. It carries **no** matching logic; every cross, fill,
    and removal is the engine's. It exposes `MeAdd` / `MeCancel` / liveness /
    best-bid-ask / depth.
  - `cmd/meadapter/wrapper.go` — `package main` + the `//export` cgo ABI. Same
    module as `internal/exchange`, so the internal import is allowed.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one call per crossing
  (go-trader is reached through cgo; the foreign-thread entry cost is amortized
  over the run). Each message is still dispatched in array order with no
  lookahead. See `docs/METHODOLOGY.md` "Batch delivery".
- For each new order: emit `OrderAck`, call `MeAdd` (which calls `orderBook.add`,
  the engine doing all crossing), then emit one `Trade` per fill (maker price,
  aggressor seq, maker/taker ids), in match order.
- Prices: workload `int64_t` ticks → `fixed.NewI(ticks, 0)` (stores `ticks*10^7`
  as fp). `NewI`/`Int()` is a strictly-increasing, exactly-invertible mapping, so
  price ordering and equality inside the book are bit-identical to the integer
  ticks; matching touches `Fixed` only through compare/Sub (never Mul/Div).
- **Timestamps**: the maker-price rule needs the aggressor strictly newer than
  every resting order, but `time.Now()` can repeat at clock resolution, so the
  shim drives a deterministic per-order monotone clock (`nextTime`) instead.
- **IOC**: submit as a limit, then the shim drops any unfilled remainder via the
  engine's own `remove()` and the adapter emits one `CancelAck` for it (the
  engine has no IOC flag).
- **Modify**: cancel + reinsert (go-trader's native `ModifyOrder` is exactly
  this); the reinsert's crossing fills are emitted after the `ModifyAck`. A
  modify of an already-FILLED order is rejected (see *Source patch*).
- **Liveness / reject adjudication + side·price echo**: the shim keeps the
  resting `sessionOrder` per id (go-trader's `allOrders` map needs the exact
  added value to remove). For a cancel or modify, the adapter consults the
  order's own engine state — the live `*Order` pointer is the same one
  `matchTrades` mutates as it fills — so a stale request against an
  already-consumed id rejects exactly as the engine's `CancelOrder` would. This
  map is **not** used for the audit queries.
- **Audit queries** read the **live engine book** (`orderBook.bids` / `.asks`
  slices and their FIFO node chains) via the shim's `MeBestBid` / `MeBestAsk` /
  `MeDepthAt`, not a shadow — so the state audit measures the engine's real
  structure.
- Order id: the harness `uint64_t` maps directly onto the engine's `OrderID`
  (`int64`); no string conversion.

## Source patch

No upstream go-trader source is patched — the matcher is used exactly as shipped
at the pin. The two added files (`internal/exchange/me_shim.go`,
`cmd/meadapter/wrapper.go`) are pure ADDITIONS in their own locations, so a
`git reset --hard <pin>` (or a fresh clone) removes them and `build.sh` restores
them each run; `build.sh` then anchor-checks that the conformance gate below is
present in the restored sources (loud-fail otherwise) and is idempotent.

**Conformance fix — reject a modify of a fully-filled order (don't swallow-ack).**
go-trader's native `ModifyOrder` is cancel-then-reinsert; when the target order
has already been fully consumed by a later aggressor, the cancel half operates on
an order that has already been removed from its price level as it filled, so the
cancel silently no-ops and the modify is ACK'd against a phantom resting order
instead of being rejected. (Contrast the cancel path, which already rejects this
case: the engine's `CancelOrder` rebuilds a fresh-time `sessionOrder` and hands
it to `orderBook.remove`, which returns `OrderNotFound` because `allOrders` is
keyed by the full `sessionOrder` value.)

Classification: hard-invariant violation (a terminal order must not be
modifiable). It is latent on the canonical workload — the report stream and the
published `normal`/seed-23 reference hash are byte-identical with or without the
fix — and surfaces only through the `stale_modify_after_full_fill` conformance
case (`scripts/conformance_check.py`): `NEW sell 10@100`, `NEW buy 10@100` (fully
fills the sell), `MODIFY` the now-filled sell → the three baselines emit a
`ModifyReject`; the as-shipped behaviour emits a `ModifyAck`.

The fix is applied in the adapter shim, symmetric with the already-correct cancel
path: the shim exposes the order's own engine state through `MeBook.MeIsActive`
(`order.IsActive()` — `orders.go`: false for Filled / Cancelled / Rejected, read
off the same live `*Order` the matcher mutated), and the wrapper's `doModify`
gates on it and emits `ME_MODIFY_REJECT` for a terminal id instead of
ack-then-reinsert. No engine source changes; quantity is conserved, the book is
never crossed. With the gate, all 14 conformance cases match the baseline
consensus (VALID ×5 across 100 seeds). Filed upstream as
[robaho/go-trader#23](https://github.com/robaho/go-trader/issues/23).

## Build / run

```bash
bash additional_references/gotrader_adapter/build.sh
./harness --engine gotrader_adapter.so --scenario normal --mode audit \
          --matcher-core 72 --drainer-core 73
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is not
   already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the right
   tarball.
2. Clones the engine into `third_party/go_trader_src/` at the pinned commit and
   `git reset --hard`s to it (no submodules are needed).
3. Copies the two added Go files into the checkout (`internal/exchange/me_shim.go`
   and `cmd/meadapter/wrapper.go`), anchor-checks the `#23` conformance gate is
   present, and builds the cgo c-shared library from the module root with
   `go build -buildmode=c-shared ./cmd/meadapter`, writing `gotrader_adapter.so`
   at the harness repo root.

Override the upstream checkout: `ME_GOTRADER_SRC=/path/to/checkout` skips the
clone (the two files are re-copied and re-anchor-checked in place each run;
`build.sh` resets that checkout to the pin first).
