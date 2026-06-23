# fmstephe_adapter — integration example

Wraps [fmstephe/matching_engine](https://github.com/fmstephe/matching_engine)
behind `api/matching_engine_api.h`. fmstephe/matching_engine is a pure-Go
price-time-priority limit-order book ("a simple financial trading matching
engine ... built to learn more about how they work").

Pinned commit: `fdc2088cfe508d78e2ec5fa6dfa2d8cb3a189873` ("Adding go.mod", repo
HEAD).

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

A `matcher.M` holds the order book — two red-black trees keyed by price (bids
and asks, each with a FIFO of orders at a price level) plus a third tree keyed
by a 64-bit `guid` for cancel-by-id, all sharing `OrderNode`s out of a
pre-sized `pqueue.Slab`. The matcher is message-driven and is driven in-process,
synchronously, on the calling thread:

```go
m := matcher.NewMatcher(slabSize)
m.Config("adapter", in, out)   // `out` is an MsgWriter; `in` is unused here
m.Submit(&msg)                 // reads msg.Kind: BUY / SELL / CANCEL
```

Native APIs visible from the adapter:

- `matcher.NewMatcher(slabSize int) *M` — pre-sizes the `OrderNode` slab.
- `M.Config(name, in, out)` — `out` (a `coordinator.MsgReaderWriter`) captures
  every report the matcher emits.
- `M.Submit(*msg.Message)` — runs the match on the calling thread. `msg.Kind`
  selects `BUY` / `SELL` / `CANCEL`; the matcher writes its output messages to
  the configured `out` synchronously before `Submit` returns.
- Order identity is `guid = CombineInt32(TraderId, TradeId) = (TraderId<<32)|TradeId`;
  there is no separate order-id. A `CANCEL` carries the same `TraderId`/`TradeId`.
- Per fill the matcher writes **two** messages via `completeTrade(brk, srk, b, s)`:
  the buy-side message `{b.TraderId, ...}` then the sell-side message
  `{s.TraderId, ...}`, both carrying the fill amount and the engine's fill price.
  A cancel writes one `CANCELLED` (resting) or `NOT_CANCELLED` (not resting).

Not provided natively: no IOC / FOK / POST-ONLY order type; no native modify;
no separate order-id (guid only); no `Accept`/`Reject` events on submit; and no
read-only best-bid/ask/depth query (the only readers are the trees themselves).
**Trade price:** the matcher prices each crossing fill at the **midpoint** of
the crossing bid and ask, not at the maker's resting price (see *Source patch*).

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `fmstephe_adapter.so` with plain C
  symbols the harness's `dlopen` resolves directly. No C++ shim. The engine is
  importable (`package matcher` / `package msg`), so the wrapper imports it
  directly through a `replace` directive `build.sh` rewrites to the pinned
  `third_party` checkout. The C structs are mirrored (byte-for-byte) inside the
  cgo preamble rather than `#include`-ing the harness header, because cgo emits
  the `engine_*` prototypes without the header's `const`-qualified pointer args
  and the C compiler would reject the mismatch.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one `Submit` per
  crossing. Without `engine_on_batch` the run measures the per-call cgo boundary
  into the Go runtime, not the matcher; each message is still dispatched in array
  order with no lookahead. See `docs/METHODOLOGY.md` "Batch delivery".
- A capturing `MsgWriter` (`captureWriter`) is wired as the matcher's `out`; the
  single matcher thread is the only writer, so it is a plain slice with no locks.
  The adapter drains it after each `Submit`.
- For each new order: emit `OrderAck` (the engine emits no accept message),
  `Submit` a `BUY`/`SELL`, then pair the matcher's `(buy, sell)` output messages
  into one harness `Trade` each — the maker is the resting order (the side
  opposite the aggressor), reported at its shadow price, with the aggressor seq.
- Order-id mapping: `order_id → TraderId = uint32(order_id)`, `TradeId = 1`, so
  `guid = (order_id<<32)|1` is unique per harness id (the harness id fits in 32
  bits). Cancels reconstruct the same guid.
- **IOC**: the engine has no IOC type. The adapter submits IOC as a normal limit,
  lets it match, then `CANCEL`s any resting remainder and emits the harness
  `CancelAck` for the dropped quantity.
- **Modify**: cancel + reinsert (no native modify). `CANCEL`, emit `ModifyAck`,
  then `Submit` a fresh order at the new price/qty (its crossing fills carry the
  modify's seq); `ModifyReject` if the order is not resting.
- **Liveness / reject adjudication + side·price echo + maker trade price** come
  from a small shadow `map[id]{price, side, remaining, alive}`. The engine's
  `CANCELLED` vs `NOT_CANCELLED` adjudicates cancel/modify success; the shadow
  supplies the side/price echoed on the acks and the **maker's resting price**
  reported on each trade (the engine's fill messages carry the midpoint, not the
  maker price — see *Source patch*). The shadow is allocated once in
  `engine_init`.
- **Audit queries** (`engine_query_best_bid` / `_best_ask` / `_depth_at`) scan
  the shadow: the engine exposes no read-only best-bid/ask/depth accessor, and
  the audit queries are rare, so an O(N) scan of the resting set is acceptable.
- The `OrderNode` slab is pre-sized once (`NewMatcher(1<<21)`): a generous
  static pool of resting nodes, the static-allocation parity a flat-array engine
  gets at init. Overflow falls back to the engine's own `Slab.Malloc` heap path.
- No hot-path heap allocation: the report buffer and shadow are sized once in
  `engine_init`; the capture slice is reused (`gCapture.msgs[:0]`); per fill the
  adapter reuses a single `me_report_t` scratch.

## Source patch

`build.sh` applies **no engine correctness patch** — the matcher source is built
**unmodified**. There are two non-engine-correctness items, documented here in
full:

**1. Trade-price convention correction (in the adapter, not the engine).**
fmstephe's matcher prices each crossing fill at the **midpoint** of the crossing
bid and ask:

```go
// matcher.go price()
func price(bPrice, sPrice uint64) uint64 {
	if sPrice == msg.MARKET_PRICE {
		return bPrice
	}
	d := bPrice - sPrice
	return sPrice + (d / 2)   // midpoint, not the maker's resting price
}
```

Price-time priority requires a marketable order to print at the **resting
(maker) price**. The midpoint only sets the *printed* price; it does **not**
affect which orders match or the resting book state — the match predicate is
`b.Price() >= s.Price()`, independent of the printed price. The adapter reports
the **maker's resting price** (known from its order shadow) on every trade — the
harness trade-price convention, the same correction the bundled baselines apply
for trade-price-convention mismatches. The midpoint behaviour is **reported as a
convention deviation in the audit, not hidden**: it is a real divergence from
price-time priority. No engine source is changed for this; the adapter simply
reports the maker price. Filed upstream as
[fmstephe/matching_engine#11](https://github.com/fmstephe/matching_engine/issues/11).
This is the "with fix" entry for fmstephe in `CONSENSUS_CONFORMING_ENGINES.md`:
the engine is consensus-CLEAN once the trade price is reported by convention
(VALID ×5 across 100 seeds).

**2. flib arm64 portability port (`third_party/fmstephe_flib_local`).** The
matcher imports `coordinator`, which imports
[`github.com/fmstephe/flib`](https://github.com/fmstephe/flib)'s spscq queue,
which imports flib's `fatomic` / `padded` / `ftime` packages. Those were written
in 2017 with **amd64-only** implementations and ship no buildable Go files for
other architectures:

- `fsync/fatomic/lazy.go` is tagged `+build amd64` → `LazyStore`.
- `fsync/padded/const_amd64.go` (implicit amd64 tag) → `CacheLineBytes`.
- `ftime/ftime.go` + `ftime_amd64.s` (amd64 asm only) → `Counter`/`cpuid`/`Pause`.

On an aarch64 host the whole module fails to compile
("build constraints exclude all Go files"). The adapter drives the matcher
**single-threaded** and never constructs an spscq queue, so this code is **dead
on our path** — it only has to compile and link. `build.sh` materialises
`third_party/fmstephe_flib_local`, a **verbatim copy** of the pinned flib with
three small arm64 port files **added** (no upstream file edited):

- `fsync/fatomic/lazy_arm64.go` — identical relaxed store to the amd64 original,
  `//go:build arm64`.
- `fsync/padded/const_arm64.go` — `CacheLineBytes = 64`.
- `ftime/ftime_arm64.s` — `Counter`=`CNTVCT_EL0`, `cpuid`=zeros, `Pause`=`ret`
  (dead code on the synchronous path; link-only).

The wrapper's go.mod `replace`s flib to this local copy. This is the same kind
of portability port the harness's `tzadiko` (Windows→POSIX `localtime`) and
`robaho` (C++20 conformance) reference adapters apply — a build-portability
change to a dependency that is dead on the measured path, **not** a change to
the matcher. On an amd64 host the upstream flib files already cover the build
and the `//go:build arm64` / `*_arm64.*` selectors make the local ports inert.

## Build / run

```bash
bash additional_references/fmstephe_adapter/build.sh
./harness --engine fmstephe_adapter.so --scenario normal --mode audit \
          --matcher-core 68 --drainer-core 69
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the
   right tarball.
2. Clones the engine into `third_party/fmstephe_matching_engine/` at the pinned
   commit and `git reset --hard`s to it (the matcher is built unmodified).
3. Materialises `third_party/fmstephe_flib_local/` — the pinned flib plus the
   three arm64 port files (see *Source patch* #2) — idempotently.
4. Rewrites the wrapper's two `replace` directives to the checkouts (relative
   paths) and builds the cgo wrapper under `wrapper/` with
   `go build -buildmode=c-shared`, writing `fmstephe_adapter.so` at the harness
   repo root.

Override the upstream checkout: `ME_FMSTEPHE_SRC=/path/to/checkout` skips the
clone (the flib local copy is still materialised under `third_party/`,
idempotently).
