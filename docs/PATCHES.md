# Upstream deviations

The harness ships adapters for three already-public matching engines —
**Liquibook**, **QuantCup**, and **Exchange-core** — that first established the
published correctness reference (the published hash is their byte-identical
consensus, since reproduced across the conforming field). `scripts/build_baselines.sh` fetches each engine from
its upstream repository at a pinned commit and builds it. This document records
every deviation from that upstream source.

Guiding rule: **the engine source is left unmodified wherever possible.**
Harness-specific glue — the C-ABI adapter, report derivation, the inter-thread
transport — lives in `adapters/`, never in the engine. A *source* patch is used
only where an engine otherwise cannot be built, or cannot express the standard
price-time-priority convention the consensus depends on.

| Engine | Pinned commit | Source patch |
|---|---|---|
| Liquibook | `2427613b32f1667abae68a01df6af9ba8270f8e7` | none |
| QuantCup | `f860e0b831a7dd2d0c07a5dbc3723ef15d1067ed` | `patches/quantcup.patch` |
| Exchange-core | `2f8548749839e9095c8dc597e4b61521d259fa5d` | none |

## Liquibook

Upstream: <https://github.com/ObjectComputing/liquibook>

**No source patch.** Liquibook is header-only apart from `simple/simple_order.cpp`;
the adapter compiles against it unmodified. All harness-specific behaviour is in
`adapters/liquibook_adapter.cpp`:

- **IOC** — the adapter builds with `LIQUIBOOK_ORDER_KNOWS_CONDITIONS` and passes
  order conditions through the `book.add(order, conditions)` argument. Liquibook's
  `OrderTracker` conditions constructor has an upstream defect (it reads the
  constructor parameter rather than the member), so the adapter routes the IOC
  flag through `add()`, the path that sets the condition correctly. No engine
  code is changed.
- **Modify** — handled as cancel + reinsert in the adapter. Liquibook's
  `replace()` is not used: it corrupts `SimpleOrder::price_`. Cancel + reinsert
  is also the modify rule the harness defines (`api/matching_engine_api.h`).
- **Reject reports** — a cancel or modify of an order that is not resting (a
  duplicate or stale message) emits a CancelReject / ModifyReject; a genuine
  cancel / modify emits a CancelAck / ModifyAck that echoes the order's side
  and price. Adapter logic — no engine code is changed.

## QuantCup

Upstream: <https://github.com/ajtulloch/quantcup-orderbook>
Patch: **`patches/quantcup.patch`** — applied automatically by
`scripts/build_baselines.sh`.

QuantCup was a 2011 contest entry; the repository contains only the contestant's
engine, not the contest's build skeleton. Three of the patch's five changes
restore that skeleton so the code compiles at all; one overrides the order-id
capacity (`kMaxNumOrders`), and one is a behavioural fix.

`scripts/build_baselines.sh` applies one further source edit, after the patch,
via `widen_quantcup_price_domain` (an idempotent Python rewrite of the
freshly-reset+patched tree): it **widens the price domain** (see the second
table below). Like the `kMaxNumOrders` override, this is a capacity change, not
a matching change — it is byte-identical on the canonical seed-23 workload.

| File | Change | Why |
|---|---|---|
| `qc_limits.h` | **new file** | `engine.h`, `types.h`, and `order_book.cpp` `#include "limits.h"` — a contest-skeleton header absent from the repository. `qc_limits.h` supplies the `<climits>` / `<cstdint>` / `<limits>` declarations QuantCup relies on. It is deliberately *not* named `limits.h`, so that QuantCup's directory on the compiler `-I` path cannot shadow the system `<limits.h>` (which `<semaphore>`, Boost, and libstdc++ pull in transitively). |
| `util.h` | **new file (empty stub)** | `engine.cpp` `#include "util.h"` — also a contest-skeleton header absent from the repository. Its C-era macros are unused in this C++ port, so the stub is empty. |
| `engine.h`, `types.h`, `order_book.cpp` | `#include "limits.h"` → `#include "qc_limits.h"` | Point at the supplied header above. |
| `constants.h` | `kMaxNumOrders` made overridable with `-DQC_MAX_NUM_ORDERS` | QuantCup's compile-time order-id capacity defaults to 101,000. The canonical workload has up to ~1,000,000 live order ids, so the adapter builds with `-DQC_MAX_NUM_ORDERS=2200000`. |
| `order_book.cpp` | `executeTrade(...)` reports each fill at the **maker's resting price** (`askMin` / `bidMax`) instead of the aggressor's limit price | **Behavioural.** QuantCup originally prints the fill at the incoming order's price. Standard price-time priority — and every other engine here — reports the fill at the *resting* order's price. Without this change QuantCup's trade prices differ and it cannot join the byte-identical consensus. |

Only the last change affects matching output; the first four are build-enabling.
The patched source still implements QuantCup's own flat price-indexed array
algorithm — the algorithm is untouched.

### Price-domain widening (`widen_quantcup_price_domain` in `scripts/build_baselines.sh`)

QuantCup's book is a flat array indexed by price, with price stored in `t_price`.
Upstream `t_price` is `unsigned short`, so the array has 65,535 slots and the
usable price domain is only `[1, 65534]` (the top value, 65535, is the array
dimension *and* the empty-ask sentinel `askMin`). A wide-swing workload whose GBM
price path walks past 65534 — e.g. `flash-crash` seed 711116612 reaches tick
68910 — would index out of the array; the adapter's `check_qc_price` guard
caught this and called `std::abort()` rather than let QuantCup silently corrupt
its book. So QuantCup crashed on those seeds instead of joining the consensus.

The script lifts the ceiling by editing the engine source (after `git reset
--hard` + `git apply`, so the edit always lands on pristine, freshly-patched
source — idempotent):

| File | Change | Why |
|---|---|---|
| `constants.h` | `typedef unsigned short t_price;` → `typedef uint32_t t_price;` | The price word widens from 16 to 32 bits, so prices above 65534 no longer truncate mod 2^16. `askMin` / `bidMax` (typed `t_price`) widen with it. |
| `constants.h` | new `constexpr t_price kNumPricePoints = 262144;` (2^18) | The flat book's dimension, decoupled from `kMaxPrice` (which is `t_price`'s max — now ~4.29e9, far too large to allocate). 262144 ticks ≈ $1310 at $0.005/tick from a $167.52 start (~8x), beyond any GBM realization; the array is ~6 MiB. Same role as the old implicit `kMaxPrice == 65535`: array size, and the empty-ask sentinel. Usable domain `[1, 262143]`. |
| `constants.h` | `kMaxLiveOrders = numeric_limits<t_price>::max()` → `= kMaxNumOrders` | `kMaxLiveOrders` aliased `t_price`'s max (65535); a 32-bit `t_price` would balloon it to 4.29e9. It is a live-order cap, not a price, so it is pinned to the arena capacity instead. (Defined-but-unused upstream; kept sane.) |
| `order_book.cpp` | `pricePoints.resize(kMaxPrice)` → `resize(kNumPricePoints)`; `askMin = kMaxPrice` → `askMin = kNumPricePoints` | Size the array and the empty-ask sentinel by the new dimension. |

`adapters/quantcup_adapter.cpp` keeps `QC_PRICE_MAX` in lockstep: it is now
`OB::kNumPricePoints - 1` (= 262143), so a price past the *widened* ceiling still
fails loud as a safety net rather than corrupting the book. None of this changes
matching on in-range prices: the canonical seed-23 workload stays well inside the
original `[1, 65534]` band, and all five seed-23 report-stream hashes remain
byte-identical to `reference/correctness_hash.txt`. Verified to **not** abort and
to match the liquibook consensus (report-stream hash + 192-point state audit) on
the previously-crashing wide-swing seeds 711116612, 1738064285, 1945965940, and
781626286.

As in the other two adapters, a cancel or modify of an order that is not
resting emits a CancelReject / ModifyReject rather than an ack — adapter logic
in `adapters/quantcup_adapter.cpp`, not a source change.

## Exchange-core

Upstream: <https://github.com/exchange-core/exchange-core>

**No source patch.** Exchange-core is a Java engine, consumed through the jar
that `mvn package` produces. The adapter is a JNI bridge:
`adapters/exchange_core_adapter.cpp` embeds a JVM and
`adapters/HarnessExchangeCore.java` drives exchange-core's `OrderBookDirectImpl`.
The adapter also exports a Java-side `engine_on_batch` (one JNI crossing per run
of messages, rather than one per message), so exchange-core is measured on its
matcher rather than on the JNI boundary. Both files are harness-owned glue; no
exchange-core class is modified.

Adapter behaviour worth recording:

- **Order book** — `OrderBookDirectImpl`, exchange-core's direct (non-naive)
  matching path, with no risk engine on the path. This measures matching in
  isolation, consistent with the other two engines and the harness's stated
  scope.
- **Modify** — the adapter performs the cancel + reinsert itself, inside
  `HarnessExchangeCore.onModify` (cancel the order, then place it anew at the
  new price/quantity). This is byte-identical, on the canonical workload, to a
  native modify, because every modify in that workload is a reprice or quantity
  increase (see `docs/METHODOLOGY.md`).
- **Reject reports** — a cancel or modify of an order that is not resting emits
  a CancelReject / ModifyReject rather than an ack. The engine itself
  adjudicates and supplies the echo: `onCancel` returns the removed order's
  side (1 bid / 2 ask, or 0 for not-resting) straight from the engine's
  id-keyed `cancelOrder` (`cmd.action`), and stages its price from the
  engine's REDUCE event; `onModify` returns −1 when the cancel half misses.
  The adapter keeps no order state of its own.
- **JVM options** — `-XX:+UseSerialGC -Xms2g -Xmx2g -XX:+AlwaysPreTouch`. A
  fixed, pre-touched 2 GiB heap and a single-threaded collector remove
  heap-resize, page-fault, and GC-pause noise from the measured pass.
- **JIT warmup** — `engine_init` runs 100,000 synthetic place / modify / IOC /
  cancel cycles (~900,000 engine calls, including the miss-path rejects and
  populated-book queries) to compile the hot path before the harness begins
  timing, mirroring exchange-core's own benchmark methodology of explicit
  warmup passes. The warmed order book is then discarded and a fresh one
  installed.
- **JDK 11** is required: exchange-core's dependency set predates later JDKs.

## Reproducing

```sh
scripts/build_baselines.sh all          # fetch, patch, and build all three
scripts/build_baselines.sh quantcup     # or just one
```

Pinned commits and the QuantCup patch are applied automatically. To build
against an existing local checkout instead of cloning, set `ME_LIQUIBOOK_SRC`,
`ME_QUANTCUP_SRC`, or `ME_EXCHANGE_CORE_SRC`.
