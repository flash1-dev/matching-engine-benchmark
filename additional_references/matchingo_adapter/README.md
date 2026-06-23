# matchingo_adapter — integration example

Wraps [GOnevo/matchingo](https://github.com/GOnevo/matchingo) behind
`api/matching_engine_api.h`. matchingo is a pure-Go price-time-priority
limit-order book ("Incredibly fast matching engine for HFT written in Golang").

Pinned commit: `7aa642f0ffc8dfd509119b1d432b8745fb1dfcc5` (tag `v0.0.1`, repo
HEAD).

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Single `OrderBook` with `Process(order) (*Done, error)` as the only entry
point, a single-threaded matcher. Native APIs visible from the adapter:

- `OrderBook.orders : map[string]*Order` — id (string) → order.
- `bids` / `asks : *OrderSide` — each a `hashicorp/go-set.TreeSet` of price
  levels (bids' comparator is reversed, so its `Min()` is the best/highest bid)
  plus `map[fpdecimal.Decimal]*OrderQueue`; the queue is a
  `gammazero/deque[*Order]` (FIFO time priority).
- `Process(order)` returns a `*Done`. `Done.Trades` is
  `[taker, maker, maker, ...]`: index 0 is the synthetic aggressor summary
  (incoming id, its own price, `Processed` qty); indices 1.. are one entry per
  resting order hit — `{maker id, fill qty, MAKER resting price}`, in match
  order. Fills cross at the **maker's** price (`processQueue` uses
  `orderQueue.Price()`).
- Native IOC / FOK via the order's `TIF`; native `CancelOrder(id) *Order`.

Not provided natively: no in-place modify (cancel + reinsert); `std::string`
order ids (the harness uses `uint64_t`); the `Done`/`CancelOrder` results do not
carry a ready-made report with the resting order's side/price, and matchingo has
no query that distinguishes a resting id from a filled/cancelled one. Prices and
quantities are `fpdecimal.Decimal` = a single `int64 v`.

## Adapter strategy

- Cgo `c-shared`: a Go `package main` with `//export` directives is built with
  `go build -buildmode=c-shared` to produce `matchingo_adapter.so` with plain C
  symbols the harness's `dlopen` resolves directly. No C++ shim. matchingo is
  importable (`package matchingo`), so the wrapper imports it directly through a
  `replace` directive `build.sh` rewrites to the pinned `third_party` checkout.
- **Batch delivery**: the adapter exports `engine_on_batch`, so the harness
  delivers a run of messages per cgo crossing instead of one `Process` /
  `CancelOrder` per crossing. Without `engine_on_batch` the run measures the
  per-call cgo boundary into the Go runtime, not the matcher; each message is
  still dispatched in array order with no lookahead. See
  `docs/METHODOLOGY.md` "Batch delivery".
- For each new order: emit `OrderAck`, `Process` a `NewLimitOrder` (GTC, or IOC
  when `o.ioc`), then emit one `Trade` per `Done.Trades[1:]` entry (maker price,
  aggressor seq, maker/taker ids), in match order.
- **IOC**: delegated via `TIF=IOC` (the engine drops the residual internally);
  the adapter emits the harness `CancelAck` for `qty − Σ fills`.
- **Modify**: cancel + reinsert (the engine has no native modify).
  `CancelOrder(id)`, then `Process` a fresh GTC order at the new price/qty; the
  reinsert's crossing fills carry the modify's seq. One `ModifyAck`, or
  `ModifyReject` if the order is not resting.
- **Liveness / reject adjudication + side·price echo** come from a small shadow
  `map[id]{price,side,remaining,alive}` (matchingo has no query that tells a
  resting id from a filled/cancelled one, and `CancelOrder` doesn't return a
  ready-made report with side/price). The shadow is allocated once in
  `engine_init` and is **not** used for the audit queries.
- **Audit queries** read the **live engine book** via the read-only
  `HarnessBestBid` / `HarnessBestAsk` / `HarnessDepthAt` accessors `build.sh`
  adds (see *Source patch*), not from the shadow — so the engine's real internal
  state (including price-level volume accounting) is what the state audit sees.
- Decimals: `fpdecimal.Decimal` wraps a single `int64 v`; `FromIntScaled(n)`
  builds `Decimal{v:n}` directly (no `*10^frac` scaling) and `Scaled()` reads it
  back. Limit/IOC matching touches decimals only through Add/Sub/compare (never
  Mul/Div — those live on the market-quote path this workload never exercises),
  so integer ticks and quantities round-trip bit-for-bit independent of
  `fpdecimal.FractionDigits`.
- Order id: `uint64 → strconv.FormatUint(.,10)` (matchingo's native
  string-keyed map; the allocation is intrinsic to the engine's
  `map[string]*Order` design, not adapter-added overhead).

## Source patch

`build.sh` applies **one engine correctness fix** and **adds one read-only
query file**. Both are applied to a pristine checkout (post `git reset --hard`),
loud-fail if their anchors move, and are idempotent (marker guards on the
override path; the reset restores pristine source on the default checkout).

**1. Price-level volume conservation after a partial fill (correctness fix).**
`OrderQueue.UpdateVolume` (`orderqueue.go:51`) subtracted the resting order's
**remaining** quantity, not the **consumed** quantity. Its only caller,
`OrderBook.processQueue` (`orderbook.go:306-308`), runs it **after** decrementing
the order:

```go
done.appendOrder(o, quantity, price)   // 'quantity' is the consumed amount
o.DecreaseQuantity(quantity)           // o.quantity := orderQty - consumed  (REMAINDER)
orderQueue.UpdateVolume(o)             // volume -= o.Quantity() == volume -= REMAINDER  (bug)
```

So when an incoming order partially fills the front resting order of a level,
the level's tracked `volume` becomes `prev − front_remaining` instead of
`prev − consumed`, and can even go negative. The **trade/report stream is
unaffected** (matching iterates `OrderQueue.Len()` / `First()`, never reads
`volume`), which is why the report-stream hash is byte-identical to the
`liquibook` baseline on all five scenarios; the corruption surfaces only through
a depth-at-price read — the harness state audit's depth check — where it fails
intermittently (it only mismatches when the audit samples a price level whose
front order was just partially consumed; ~1 in 4 `normal` runs as shipped).
It would also corrupt `CanOrderBeFilled` (FOK) and `CalculateMarketPrice`, which
read level volume — not exercised by this workload.

Classification: hard-invariant violation (resting-depth non-conservation). The
fix passes the **consumed** quantity to `UpdateVolume` explicitly and subtracts
that: `UpdateVolume(consumed fpdecimal.Decimal) { oq.volume = oq.volume.Sub(consumed) }`,
with the single call site changed to `orderQueue.UpdateVolume(quantity)` placed
before `DecreaseQuantity`. Two edits; no behaviour change beyond the volume
accounting, so every report hash still matches the baseline and the state audit
now passes deterministically (VALID ×5 across 100 seeds). Filed upstream as
[GOnevo/matchingo#1](https://github.com/GOnevo/matchingo/issues/1).

**2. Read-only audit accessors (`harness_query.go`).** matchingo exposes no
numeric best-bid/ask/depth accessor the audit needs — its only public reader,
`OrderBook.Depth()`, `fmt.Println`s on every call and returns
`map[string]string`. `build.sh` drops in **one new file**, `harness_query.go`,
into the engine package (same `package matchingo`, so it reads the unexported
`bids`/`asks`/`prices` fields), modifying no existing source. It exports three
read-only, side-effect-free accessors used by `engine_query_*`:

- `HarnessBestBid() (int64, bool)` / `HarnessBestAsk() (int64, bool)` — best
  price from the side's price tree.
- `HarnessDepthAt(scaledPrice int64, side Side) int64` — the price level's own
  `volume` field.

No matching logic is touched; the accessors read the live engine book exactly as
the engine maintains it, so the engine's real depth accounting (fix #1 included)
is surfaced to the state audit rather than hidden behind an adapter shadow.

## Build / run

```bash
bash additional_references/matchingo_adapter/build.sh
./harness --engine matchingo_adapter.so --scenario normal --mode audit \
          --matcher-core 56 --drainer-core 57
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH`. No sudo. Detects `aarch64` vs `x86_64` and picks the
   right tarball.
2. Clones the engine into `third_party/matchingo_src/` at the pinned commit,
   `git reset --hard`s to it, applies the volume-conservation fix, and drops in
   `harness_query.go`.
3. Rewrites the wrapper's `replace` directive to the checkout (relative path)
   and builds the cgo wrapper under `wrapper/` with
   `go build -buildmode=c-shared`, writing `matchingo_adapter.so` at the harness
   repo root.

Override the upstream checkout: `ME_MATCHINGO_SRC=/path/to/checkout` skips the
clone (the source patch and query file are re-applied in place, idempotently).
