# geseq_adapter — integration example

Wraps [geseq/orderbook](https://github.com/geseq/orderbook) behind
`api/matching_engine_api.h`. geseq/orderbook is a pure-Go limit-order book
with a callback-style `NotificationHandler` interface.

Pinned commit: `88e80980c691bcb62be8bd59ef9b2c04706e7c51` (past the upstream fix
for the price-cross defect we reported — see *Engine issue* below).

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Single-symbol `OrderBook` with a per-call monotonic token (`tok`) that
`AddOrder` / `CancelOrder` / `Bid` / `Ask` `CompareAndSwap` against an internal
`lastToken` for determinism. Native APIs visible from the adapter:

- `OrderBook.AddOrder(tok, id, class, side, qty, price, trigPrice, flag)` —
  submits a Limit/Market with optional IoC/FoK/AoN/StopLoss/TakeProfit flag.
- `OrderBook.CancelOrder(tok, id)` — removes a resting order.
- `OrderBook.Bid(tok) / Ask(tok)` — peek best-side order (token-consuming).
- `NotificationHandler` interface: `PutOrder(MsgType, OrderStatus, ...)` for
  Accept / Reject / Cancel, `PutTrade(maker, taker, ..., qty, price)` per fill.

Not provided natively: no in-place modify (`README.md` notes "No in-book
updates. Updates will have to be handled with Cancel+Create"); the
`NotificationHandler.PutOrder` callback carries `MsgType + OrderStatus +
orderID + qty` but no side or price, so the harness wire format's CancelAck /
ModifyAck cannot be reconstructed from the callback alone.

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built
  with `go build -buildmode=c-shared` to produce `geseq_adapter.so` with
  plain C symbols the harness's `dlopen` resolves directly. No C++ shim.
- **Batch delivery**: the adapter also exports `engine_on_batch`, so the
  harness delivers a run of messages per cgo crossing instead of one
  `AddOrder` / `CancelOrder` per crossing. Geseq's published figure is the
  batched one; without `engine_on_batch` the run measures the per-call cgo
  boundary into the Go runtime, not the matcher. See `docs/METHODOLOGY.md`
  "Batch delivery".
- A `harnessHandler` implements `NotificationHandler`. `PutTrade` emits the
  harness Trade report (carrying `gCurSeq`, the per-call seq the adapter
  sets before each `AddOrder`) and decrements the maker's shadow remainder.
  `PutOrder` records the engine's cancel verdict for `MsgCancelOrder`
  (`Canceled`/`Rejected` + the engine-reported remaining quantity) — the
  adapter adjudicates cancels and the modify's cancel half from it — and
  ignores create notifications. The adapter synthesises OrderAck / CancelAck /
  ModifyAck / CancelReject / ModifyReject above the engine, with side and
  price echoed from the shadow.
- **Token**: a monotonic per-process counter (`gTok`) advanced before every
  `AddOrder` / `CancelOrder` (and twice per modify, since a modify is
  cancel + reinsert).
- **IoC**: delegated via `flag = IoC`. The engine drops the residual without
  emitting a cancel notification, so the adapter detects the residual from
  `qty - sum_of_trades` and emits the harness `CancelAck` itself.
- **Modify**: cancel + reinsert (the engine has no native modify). The
  reinsert's fills carry the modify's seq through `gCurSeq`.
- **Audit queries** read from the shadow map, not from `OrderBook.Bid()` /
  `Ask()`. The engine's Bid/Ask peeks `CompareAndSwap` the same `lastToken`
  that `AddOrder` / `CancelOrder` use, so calling them from the audit path
  would burn a token slot and desynchronise the next hot-path dispatch.
- Decimals: workload `int64_t` ticks carried as
  `udecimal.New(uint64(tick), 0)`, which produces an internal fp of
  `tick * 10^8`. The engine compares fp directly, so tick ordering is
  preserved bit-for-bit; `d.Int()` recovers the tick exactly on the way back.

## Engine issue (resolved upstream)

No source patch. `pricelevel.go::processLimitOrder` once matched the best-price
queue and then iterated without re-checking the price-cross predicate, so an
aggressor with quantity left over kept consuming non-crossing levels. That was
reported as
[geseq/orderbook#25](https://github.com/geseq/orderbook/issues/25) and fixed
upstream — the loop now adds `&& compare(orderQueue.Price())` to its header. The
pinned commit above contains the fix, so the engine builds unmodified. Full
analysis and resolution are in `../../RESOLVED_FINDINGS.md`.

## Build / run

```bash
bash additional_references/geseq_adapter/build.sh
./harness --engine geseq_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go`
   is not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and
   picks the right tarball.
2. Clones the engine into `third_party/geseq_orderbook/` at the pinned
   commit, which already contains the upstream fix (no patch).
3. Builds the cgo wrapper under `wrapper/` with `go build
   -buildmode=c-shared`, writing `geseq_adapter.so` at the harness repo
   root.

Override the upstream checkout: `ME_GESEQ_SRC=/path/to/checkout` skips the
clone. Point it at a commit that contains the upstream fix (`88e8098` or later).
