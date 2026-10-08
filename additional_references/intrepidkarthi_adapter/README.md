# intrepidkarthi_adapter — integration example

Wraps [intrepidkarthi/orderbook](https://github.com/intrepidkarthi/orderbook) behind
`api/matching_engine_api.h`. It is a Go limit order book and matching engine:
integer `int64` ticks and lots, one single-writer engine per book, and an ordered
event stream.

Pinned commit: `eaf40498e09bed01779521592a583bcff4d09d6e`.

The adapter is maintained upstream, in `cmd/flash1engine` (cgo glue) and
`internal/flash1` (the mapping, in plain Go with unit tests). `build.sh` builds it
from the pinned commit, so nothing is vendored here.

## Engine shape

`matching.Engine` is one book, owned by one goroutine. Native APIs the adapter uses:

- **`Engine.Match(order, buf)`** submits a limit order, GTC or IOC. It fills a
  caller-owned trade buffer and allocates nothing for the trades.
- **`Engine.Cancel(id, user)`** removes a resting order.
- **`Engine.Replace(id, user, order)`** is cancel and re-add, losing priority.
- **Events.** `EventSink` receives every event in sequence order: accepted, trade,
  canceled, replaced.
- **Book reads.** `Engine.BestBid` / `BestAsk`, and the book's per-price order queues
  for `depth_at`.

Self-trade prevention is configurable. The adapter sets `STPAllow`, because the
workload has no accounts and every order belongs to one user.

## Adapter strategy

- **cgo `c-shared`.** `cmd/flash1engine` is a Go `package main` with `//export`
  functions. The ABI structs are declared in its own preamble from the header's
  documented layout, with `_Static_assert` on every size and the offsets it reads.
- **Batch delivery.** It exports `engine_on_batch`, so one cgo crossing carries a
  run of messages.
- **Reports cross outbound once per call or batch.** They are buffered in a Go slice
  and handed to the transport in a single C call.
- **Report mapping:**
  - **new order:** `OrderAck`, then one `Trade` per fill at the maker's price. For
    IOC, a `CancelAck` for the unfilled residual.
  - **cancel:** a `CancelAck` with the order's side, price and remaining quantity,
    or `CancelReject` if it is not resting.
  - **modify:** `Engine.Replace`. `ModifyAck` with the new price and quantity, then
    the re-added order's trades under the modify's sequence number. `ModifyReject`
    if the order is not resting.
- **Ids.** The engine assigns its own order ids, and the adapter keeps the map in both
  directions. A client id freed by a fill or a cancel can be reused.
- **Audit queries read the engine's own book**, never a shadow: `BestBid` / `BestAsk`,
  and depth as the sum of remaining quantity in that side's queue at the price.

## Verification

Run on GitHub Actions against this harness at
`60049226c1a1dad127a50a7c12d62baf0377fbaa`, where every third-party engine is built.
The workflows are `flash1.yml` and `flash1-conformance.yml` upstream.

- **Canonical scenarios:** the report hash equals the consensus on all five, verdict
  VALID on each.
- **`scripts/conformance_check.py`:** `CONFORMANT`, with the report stream matching on
  34 cases and book state on 33.
- **`--mode audit`:** all 192 state checks match Liquibook, on each of the five
  scenarios.
- **Seed sweep:** seeds 1 to 20 on all five scenarios, 100 runs. Every one is
  byte-identical to Liquibook's canonical output for the same seed and scenario.

The run, with its job summary:
<https://github.com/intrepidkarthi/orderbook/actions/runs/37646190523>.

## Build / run

```bash
bash additional_references/intrepidkarthi_adapter/build.sh
./harness --engine ./intrepidkarthi_adapter.so --scenario normal --mode audit \
          --matcher-core 2 --drainer-core 3
```

`build.sh`:
1. Installs a Go toolchain (1.23.5) under `third_party/go-1.23.5/` if `go` is not
   already on `PATH`. No sudo.
2. Clones the engine into `third_party/intrepidkarthi_orderbook/` at the pinned
   commit.
3. Builds `./cmd/flash1engine` with `go build -buildmode=c-shared`, writing
   `intrepidkarthi_adapter.so` at the harness repo root.

Override the checkout with `ME_INTREPIDKARTHI_SRC=/path/to/checkout`.

License: MIT.
