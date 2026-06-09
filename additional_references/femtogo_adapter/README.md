# femtogo_adapter — integration example

Wraps [ejyy/femto_go](https://github.com/ejyy/femto_go) behind
`api/matching_engine_api.h`. femto_go is a pure-Go, multi-symbol,
price-time-priority limit order book built around two SPSC ring buffers (an
input command ring and an output event ring).

Pinned commit: `46667a95064bd028e8f0ec1bc6a2f776d86721e3`.

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `discoveries.md` at the repository
root for the observations the harness produced against this snapshot.

## Engine shape

`package main`, ~500 SLOC, zero dependencies. A `MatchingEngine` holds
`[MAX_SYMBOLS]OrderBook`; each `OrderBook` is two flat arrays of
`[MAX_PRICE_LEVELS]PriceLevel` (intrusive FIFO queues) indexed directly by
price. Native API visible to a driver:

- `MatchingEngine.Limit(symbol, side, price, size, trader)` — submit a limit
  order. The engine assigns its **own** monotonic `OrderID` (`e.orderID++`);
  the caller cannot supply one. Pushes an `ORDER_EVENT` then one
  `EXECUTION_EVENT` per fill onto `outputRing`. No IoC / FoK flag.
- `MatchingEngine.Cancel(orderID)` — cancel a resting order by the engine's
  id. Pushes `CANCEL_EVENT` on success, a generic `REJECT_EVENT` on failure.
- No native modify (`README` "Updates will have to be handled with
  Cancel+Create").
- Results are `OutputEvent`s on `outputRing`; `main.go`'s demo drains them on
  a separate goroutine via `StartOutputDistributor`.

Constraints that shape the adapter:

- `Price`, `OrderID`, `Size` are `uint32`; `price` is used directly as an
  array index and must satisfy `0 < price < MAX_PRICE_LEVELS` (= 16384), or
  the order is rejected. The harness workload ticks are ~32k.
- Event vocabulary is coarser than the harness's: `ORDER` / `CANCEL` /
  `EXECUTION` / a single generic `REJECT` — and the `REJECT_EVENT` carries
  **no order id**. The six harness report types (and CancelReject vs
  ModifyReject) must be synthesised above the engine.

## Adapter strategy

- **Cgo `c-shared`**: a Go `package main` with `//export` directives is built
  with `go build -buildmode=c-shared` to produce `femtogo_adapter.so` with
  plain C symbols the harness's `dlopen` resolves directly. No C++ shim.
- **Engine compiled in, not imported.** femto_go is `package main` with
  unexported types (`Order`, `OrderBook`, `Side`, `Price`, …) and an
  unexported `outputRing` field, so it cannot be imported as a library.
  `build.sh` copies the pinned-SHA engine `.go` files (everything except
  `main.go` and `*_test.go`) into `wrapper/` as `femto_*.go` so they compile
  together as one package, giving the wrapper direct access to
  `MatchingEngine`, `Limit`/`Cancel` and `outputRing`. **No engine source is
  modified — there is no patch.** (`main.go` is excluded because its `func
  main()` would collide with the `buildmode=c-shared` `func main()`.)
- **Synchronous, no goroutine.** The adapter does not start the engine's
  input/output distributor goroutines. It calls `Limit`/`Cancel` inline on the
  harness matcher thread and drains `outputRing` immediately. `RingBuffer.Read`
  *spins* on an empty ring, so it cannot be used to "drain whatever is there";
  the adapter reads `writePos`/`readPos` directly (safe — producer and
  consumer are the same thread) and pulls out exactly the events the
  just-finished call produced. `engine_flush()` is therefore a no-op.
- **Engine-assigned ids.** The engine ignores any caller id and assigns its
  own; cancels take that engine id. The adapter maps harness_oid ↔ engine_oid
  both ways: engine→harness to translate a trade's maker/taker ids back to
  harness ids, harness→engine to drive `Cancel` and the modify's cancel step.
  The engine id of a new order is read out of the `ORDER_EVENT` the engine
  emits first (a rejected order does **not** consume an engine id, so reading
  the event is exact).
- **Price remap.** Every harness tick is shifted by a fixed offset
  (`START_MID` 33504 ticks − the 8192 window midpoint) so the workload band
  lands in the middle of the engine's `[1, 16383]` index space. The map is
  strictly increasing and injective, so matching order is unchanged; `d.Int()`
  has no analogue here — the inverse is a plain add-back. The normal band
  (~1.8k ticks wide, min 31994 / max 33785 at the default seed) sits inside
  the window with ~6k ticks of head-room each side. A tick that would map
  outside `[1, 16383]` is treated as out of range and rejected the way the
  engine itself rejects an out-of-range price.
- **Shadow map** `{harness_oid -> engine_oid, side, price, remaining, alive}`
  is the source of truth for the reject path, the side/price echo the engine's
  events drop, and the audit queries.
- **IoC**: the engine has no IoC flag, so the adapter submits the order GTC,
  sees how much filled (`qty − Σ fills`), and if a residual rests it issues an
  engine `Cancel` to pull it back out and emits the harness residual
  `CancelAck` itself.
- **Modify**: cancel + reinsert (the engine has no native modify). The
  reinsert's fills carry the modify's seq; exactly one `ModifyAck` is emitted.
- **Audit queries** scan the shadow map (O(N), but they fire only at the
  harness's rare probe points and are excluded from the timed window).

## Result (the headline)

Against `normal` the adapter is **VALID** — the report stream reproduces the
canonical hash byte-for-byte (every report type: OrderAck, Trade, CancelAck,
ModifyAck, CancelReject, ModifyReject, IoC-residual CancelAck). But the
measured throughput **collapses to ~0.01 M msgs/s** (~175 µs/message; ~2M
messages in ~350 s on shared cores).

The README advertises **">10M orders/second, ~70 ns/order (Apple M1)"**. That
figure is an *in-process* number: `main.go` drives the engine from one
goroutine and "drains" the output ring on another goroutine **in the same
process**, where a report hand-off is a couple of atomic stores and the
consumer does nothing but count. The harness instead makes the engine emit
**every** report across a real inter-thread boundary (the harness transport,
drained on another core) through the C ABI. For a Go engine loaded via
`dlopen`, that means each report is a cgo call out of the Go runtime made from
the harness's foreign (C, pinned) matcher thread, and the per-order shadow
bookkeeping churns the Go GC (the run spawns ~100 runtime threads, ~700 MB
RSS). Micro-benchmarks isolate where the cost is NOT: the engine alone in the
harness's narrow-band, 90%-cancel regime runs at ~40 M/s in pure Go, and even
with the adapter's two shadow maps added it is still ~9 M/s — three to four
orders of magnitude above the measured 0.01 M/s. The remaining, dominant cost
is the cgo / cross-thread report-emission boundary the in-process "10M/s"
number never pays. This is the same lesson the harness was built to surface:
an engine whose benchmark reports through in-process callbacks hides the
dominant inter-thread reporting cost.

## Build / run

```bash
bash additional_references/femtogo_adapter/build.sh
./harness --engine ./femtogo_adapter.so --scenario normal --mode perf \
          --matcher-core 86 --drainer-core 87
./harness --engine ./femtogo_adapter.so --scenario normal --mode audit \
          --matcher-core 86 --drainer-core 87
```

`build.sh`:
1. Installs a Go toolchain (1.23.4) under `third_party/go-1.23.4/` if `go` is
   not already on `PATH` (reusing the one the geseq adapter installs, if
   present). No sudo. Detects `aarch64` vs `x86_64`.
2. Clones the engine into `third_party/femto_go/` and `git reset --hard`s the
   pinned commit (idempotent — a rerun starts clean).
3. Copies the engine `.go` files (minus `main.go` and tests) into `wrapper/`
   as `femto_*.go`, then builds the cgo wrapper with `go build
   -buildmode=c-shared`, writing `femtogo_adapter.so` at the harness repo
   root.

Override the upstream checkout: `ME_FEMTOGO_SRC=/path/to/checkout` skips the
clone. The copied `femto_*.go` files are git-ignored (regenerated each build).

### Build flags

Go's `c-shared` build mode needs no explicit host-CPU flag (per
`docs/INTEGRATION.md`); the recipe is the plain `go build -buildmode=c-shared`
(`-ldflags="-s -w"` only strips the symbol table — no optimisation change).

### Note on robustness

The adapter targets the single-book `normal` workload. A single aggressive
order produces at most ~100 fills there, far under the engine's 65536-element
output ring, so the inline drain after every call keeps the ring from ever
filling (the engine's `Push` would otherwise spin if a single call overflowed
the ring with nothing draining it mid-call). The volatile scenarios can drift
the mid far enough to push prices outside the `[1, 16383]` window; the adapter
rejects those defensively (order-preservingly) rather than wrapping the index,
so it is faithful on `normal` but not validated on the wide-swing scenarios.
```
