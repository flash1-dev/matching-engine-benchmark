# Upstream deviations

The harness ships adapters for three already-public matching engines —
**Liquibook**, **QuantCup**, and **Exchange-core** — used as the reference
baselines and as the correctness oracle (the published hash is the byte-identical
consensus of all three). `scripts/build_baselines.sh` fetches each engine from
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
engine, not the contest's build skeleton. Four of the patch's five changes
restore that skeleton so the code compiles at all; one is a behavioural fix.

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
