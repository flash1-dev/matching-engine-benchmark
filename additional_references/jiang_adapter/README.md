# jiang_adapter — integration example

Wraps [JiangYongKang/FastMatchingEngine](https://github.com/JiangYongKang/FastMatchingEngine)
behind `api/matching_engine_api.h`. FastMatchingEngine is a dependency-free
Java digital-currency matching-engine POC; the harness is native, so this
adapter embeds a JVM (JNI) and drives the engine's `OrderBook` through a small
Java helper (`HarnessJiang`).

Pinned commit:
- `JiangYongKang/FastMatchingEngine` — `8a3b597a042e402cd8bd5c95fc2d3b0884913022`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot (jiang is
**conforming with a one-line fix** — see *Source patch* below).

## Engine shape

`com.fast.matching.engine.OrderBook` is the matcher: a price-time-priority CLOB
with a `TreeMap<BigDecimal, OrderBucket>` per side (ask = natural order, bid =
reverse order), each bucket a FIFO `LinkedHashMap<Long, Order>`, plus a
`HashMap<Long, Order>` id index (`idMaps`). Native APIs the adapter uses:

- `OrderBook.newOrder(id, uid, price, volume, action, timestamp)` — the engine's
  own price-time matcher: crosses the incoming order against the best contra
  buckets, then rests any residual. It returns the trade list and accumulates it
  in `OrderBook.trades()`; a crossing `Trade` carries `targetOrderId` (the maker
  / resting order), `sourceOrderId` (the taker / aggressor), `commissionPrice`
  (the maker's resting price) and `commissionVolume` (the fill size) — exactly
  the harness `ME_TRADE` semantics.
- `OrderBook.cancelOrder(id)` — the engine's own removal (post-fix; see below).
- `OrderBook.getOrder(id)` — read-only id-keyed lookup over `idMaps` (added by
  the build's PATCH 3) for liveness / price / side.
- `OrderBook.bidOrderBucket()` / `askOrderBucket()` (the per-side `SortedMap`s)
  and `OrderBucket.volume()` for the audit queries (best bid/ask, depth-at).

Not provided natively: no IOC / FOK / POST-ONLY order type (`newOrder` always
rests a residual); no native modify; a self-trade is *suppressed* when
`source.uid().equals(target.uid())` (`OrderBucket.doExchange`).

## Adapter strategy

- **Reports**: `HarnessJiang` writes one fill record per `Trade` produced by a
  `newOrder` / `onModify` call into an adapter-owned direct `ByteBuffer` (the
  single matcher thread is the only writer); the native side turns each into a
  `ME_TRADE` and adds the `OrderAck` / `CancelAck` / `ModifyAck` (and
  `CancelReject` / `ModifyReject`) reports, pushing all of them through the
  harness transport. The slice produced by one call is taken via the
  `OrderBook.trades()` size delta (before/after), not by re-matching.
- **Order ids**: the harness order id is passed straight through as the engine's
  `id`, and *also* as the order's `uid`. The engine suppresses a fill between two
  orders with the same `uid` (its self-trade guard), but the harness has no
  notion of a user and every order must be able to match every other; giving each
  order a unique `uid` (its own id) means two distinct orders never collide, so
  matching is never suppressed. The maker/taker ids come straight off the
  engine's own `Trade` (`targetOrderId` / `sourceOrderId`).
- **Maker price**: read directly from the engine's `Trade.commissionPrice()` —
  no adapter-side shadow of the book is kept. Liveness / price / side for a
  cancel come from the engine's own `getOrder(id)`.
- **Prices**: workload `int64_t` ticks go into `BigDecimal.valueOf(price)`;
  trade and query prices come back via `longValueExact()`. The engine compares
  prices with `BigDecimal`'s ordering, so tick order is preserved exactly.
- **IOC**: the engine has no IOC type, so an IOC new order is matched by
  `newOrder` and, if any residual rested, the helper pulls it back out via the
  engine's own `cancelOrder`; the native side emits the residual `CancelAck` from
  `filled < quantity`.
- **Modify**: cancel + reinsert (the harness rule) — `getOrder` tests that the
  order is resting, `cancelOrder` removes it, then `newOrder` re-adds it at the
  new price/qty with fresh time priority, emitting each crossing fill plus one
  `ModifyAck`; a not-resting id is a `ModifyReject`.
- **Cancel adjudication**: `getOrder(id)` *is* the resting test — a hit is acked
  (price/side echoed from the returned `Order`) and `cancelOrder` removes it; a
  miss (already filled / already cancelled / never seen) is a `CancelReject`.
- **JVM**: `engine_init` (on the harness matcher thread) `dlopen`s `libjvm.so`,
  creates the VM with SerialGC and a pre-touched fixed 2 GiB heap (removes
  GC / heap-resize / page-fault noise from the measured pass), constructs
  `HarnessJiang`, hands it the adapter-owned staging buffer, and warms the JIT on
  a throwaway book that is then discarded. Every `engine_*` call runs on that
  same thread, so one cached `JNIEnv` is valid throughout and `engine_flush` is a
  no-op (the engine matches synchronously). No `engine_prebuild` is exported.

## Source patch

`build.sh` applies three minimal, idempotent patches to the engine's
`OrderBook.java` (each guarded by a marker so a re-apply is a no-op, and
anchor-checked so an upstream change can't silently no-op the fix). No matching
logic is changed by any of them.

1. **`cancelOrder` id-index prune** — add `idMaps.remove(id)`. **Real bug fix**,
   reported upstream as
   [JiangYongKang/FastMatchingEngine#3](https://github.com/JiangYongKang/FastMatchingEngine/issues/3).
   `cancelOrder` removed the order from its price bucket but never pruned the id
   index, so a cancelled id was permanently *burned*: re-adding it was silently
   dropped (`newOrder` rests only an id not already in `idMaps`) and a second
   cancel of it threw a `NullPointerException` (`bucketMap.get(...)` returned
   null after the now-empty bucket had been removed). This breaks the modify path
   entirely (modify = cancel + reinsert under the same id) and crashes on a stale
   re-cancel. The fix is the one line the upstream issue proposes.
2. **`newOrder` match-loop prune** — the same burned-id defect on the *match*
   path: `OrderBucket.doExchange` removes a fully-filled maker from its bucket but
   never from `idMaps`, so a later cancel/modify of that maker would `NPE` the
   same way. After each `doExchange`, any returned-trade maker whose remaining
   volume is now zero is removed from `idMaps`. The maker `Order` in `idMaps` is
   the same reference held by the bucket, so the remaining-volume test is exact;
   this only keeps the id index in sync with the book.
3. **`getOrder` accessor** — a one-line read-only `getOrder(Long id)` returning
   `idMaps.get(id)`. The harness cancel/modify carry only an order_id, but the
   helper needs a not-resting test (for `CancelReject` / `ModifyReject`) and the
   resting order's price/side (for the `CancelAck`); `cancelOrder` itself NPEs on
   a non-resting id. With patches 1+2 making `idMaps` authoritative, this lets the
   helper ask the engine its own state instead of keeping a parallel adapter
   shadow. Pure observation — no matching logic touched.

Patches 1 and 3 keep the engine's removal/lookup correct and crash-free; patch 2
keeps the id index consistent on the match path so patches 1+3 stay correct after
fills. With the fix the engine is VALID across all five scenarios.

## Build / run

```bash
bash additional_references/jiang_adapter/build.sh
./harness --engine jiang_adapter.so --scenario normal --mode audit \
          --matcher-core 58 --drainer-core 59
```

`build.sh` clones FastMatchingEngine into `third_party/jiang_FastMatchingEngine/`
at the pinned commit, applies the three `OrderBook.java` patches, compiles the
five engine sources + `HarnessJiang` into `third_party/jiang_build/classes/`,
writes `jiang.classpath` at the repo root, and compiles `jiang_adapter.so` at the
repo root (with the JDK's `libjvm.so` path baked in). All generated output lands
under the gitignored `third_party/` tree; the adapter directory itself holds only
the authored `jiang_adapter.cpp`, `HarnessJiang.java`, `build.sh`, and this
README. The adapter reads `./jiang.classpath` at run time and embeds the JVM via
that classpath.

Requires a JDK 21 (`build.sh` auto-installs `openjdk-21-jdk-headless` via `apt`
only if no JDK is found; the engine targets Java 1.8 but any modern JDK builds and
runs it). Overrides: `ME_JIANG_SRC` uses an existing FastMatchingEngine checkout
in place of cloning; `ME_JDK` selects a specific JDK.
