# cointossx_adapter — integration example

Wraps [dharmeshsing/CoinTossX](https://github.com/dharmeshsing/CoinTossX) behind
`api/matching_engine_api.h`. CoinTossX is a full Java JSE exchange built on
Aeron/Agrona messaging; the harness is native, so this adapter embeds a JVM (JNI)
and drives CoinTossX's *matcher* — without Aeron, UDP, or the disruptor in the
loop — through a small Java helper (`HarnessCoinTossX`).

Pinned commit:
- `dharmeshsing/CoinTossX` — `89090edcd15a06f4ed821890adfc8f377ed7d7c7`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot (CoinTossX is
**conforming with fix** — two matching-bug fixes were required and submitted
upstream as [issue #10](https://github.com/dharmeshsing/CoinTossX/issues/10)).

## Engine shape

CoinTossX's matching engine is a separable in-process core: a per-security
`orderBook.OrderBook` (a custom B+Tree price index over off-heap
`sun.misc.Unsafe` order lists) crossed by
`crossing.tradingSessions.ContinuousTradingProcessor.process(OrderBook,
OrderEntry)` under the price-time-priority strategy. The helper drives that core
directly — exactly as CoinTossX's own `ContinuousTradingProcessorTest` does.
Native APIs the adapter uses:

- `TradingSessionProcessor.process(OrderBook, OrderEntry)` — the engine's own
  continuous-trading matcher (aggress against the best contra levels price-time,
  then rest the residual).
- `leafNode.OrderEntryFactory.getOrderEntry()` — the engine's off-heap order node.
- The engine's own off-heap level + B+Tree removal (the path
  `CancelOrderPreProcessor` walks: scan the price level, drop the entry whose
  `clientOrderId` matches, prune the empty level) for cancel/modify.
- `OrderBook.getBestBid()` / `getBestOffer()` and `getBidTree()` / `getOfferTree()`
  (whose `OrderList.total()` is the resting depth at a price) for the audit
  queries.

Not provided natively: no IOC / FOK / POST-ONLY order type and no native modify
(both are synthesized by the adapter); CoinTossX's cancel needs the order's
**price and side** to locate it (`CancelOrderPreProcessor` looks up
`tree[price]` on the order's side) and returns no "unknown order" code (a missing
cancel is silently a no-op); and its fill record
(`ExecutionReportData.addFillGroup(price, qty)`) collapses same-price fills into a
map and keeps **no counterparty ids**.

## Adapter strategy

- **Order ids**: the harness `uint64_t` id is the order's `clientOrderId`
  (`OrderEntry.setClientOrderId`), which is the durable id CoinTossX cancels on
  and the id the per-fill sink reports — no id translation table is needed.
- **Reports**: the engine's matcher produces fills; `HarnessCoinTossX` writes one
  fill record per fill into an adapter-owned direct `ByteBuffer` (the single
  matcher thread is the only writer), and the native side turns each into a
  `ME_TRADE` and adds the `OrderAck` / `CancelAck` / `ModifyAck` (and
  `CancelReject` / `ModifyReject`) reports, pushing all of them through the
  harness transport.
- **Maker price**: CoinTossX's fill record keeps no counterparty ids and no maker
  price, so the helper keeps a per-order liveness/price/side shadow
  (`order_id → {price, side, remaining}`, an hppc `LongObjectHashMap`) and the
  patched per-fill sink (below) supplies the maker/taker ids. This is the same
  minimal state the C++ reference adapters keep for the same reason; it never
  matches and never drives priority. A partial reduction is tracked (decremented
  in the fill hook) so a later cancel/modify of the same id still reports the
  right resting price/side and a stale cancel/modify of a fully-filled order is
  correctly rejected.
- **Time priority**: CoinTossX orders each price level by
  `OrderEntry.getSubmittedTime()`; the harness expresses time priority as arrival
  order and carries no timestamp, so the helper stamps a strictly increasing
  counter (equal timestamps would break FIFO in the level's binary insertion).
- **IOC**: CoinTossX has no IOC type, so an IOC new order is matched by
  `process()` and any residual is dropped by the adapter, which emits the residual
  `CancelAck` (the order is created with `TimeInForce.IOC` and never rested in the
  shadow).
- **Modify**: cancel + reinsert (the harness rule) — remove the resting order via
  the engine's own level/B+Tree removal, then re-add at the new price/qty with
  fresh time priority, emitting each crossing fill plus one `ModifyAck`, or a
  `ModifyReject` if the order was not resting.
- **Cancel adjudication**: the shadow's presence is the resting test — a hit is
  acked (price/side echoed from the shadow), a miss (already filled / already
  cancelled / never seen) is a `CancelReject`. The engine's own removal returning
  "nothing removed" is caught as a backstop.
- **JVM**: `engine_init` (on the harness matcher thread) `dlopen`s `libjvm.so`,
  creates the VM with SerialGC and a pre-touched fixed 2 GiB heap (removes
  GC/heap-resize/page-fault noise from the measured pass — the off-heap order
  nodes live outside this heap), disables the circuit breaker / auctions (pure
  continuous trading), constructs `HarnessCoinTossX`, and warms the JIT on a
  throwaway book that is then discarded (`newEngine()` leaves no state behind).
  Every `engine_*` call runs on that same thread, so one cached `JNIEnv` is valid
  throughout and `engine_flush` is a no-op (CoinTossX matches synchronously on the
  calling thread). No `engine_prebuild` is exported.

## Source patch

CoinTossX requires **two real matching-bug fixes** (submitted upstream as
[dharmeshsing/CoinTossX#10](https://github.com/dharmeshsing/CoinTossX/issues/10)),
plus **one observation-only hook** the harness Trade report needs. `build.sh`
applies all three to the cloned engine tree after `git reset --hard <pin>`, each
idempotently and behind an anchor check that fails loudly if upstream moves the
code out from under it.

1. **`BPlusTree.getFirstKey()` — destructive-read fix (matching bug).**
   `OrderBook.getBestBid()` / `getBestOffer()` read `bidTree`/`offerTree`
   `.getFirstKey()`, which delegated to `root.firstKey()`. For a *leaf* root (book
   depth ≤ `nodeSize` = 100 levels) that is the smallest key, but once a side
   holds > 100 price levels the root is a `Branch`, and `Branch.firstKey()` is
   **not** a "smallest key" accessor — it is a one-shot **destructive** reader of
   the transient split-key slot used only while a split propagates up (null on a
   settled root). So `getFirstKey()` returned `null` on every deep book (the leaf
   chain and descent index are intact — `get()` and the iterator both work; only
   this best-price read is wrong), `getBestBid()` / `getBestOffer()` collapsed to
   0, and marketable orders rested instead of crossing → crossed/locked book. The
   fix descends the leftmost-child path to the first leaf and returns its smallest
   key, using only non-destructive accessors; no split/merge/rebalance logic is
   touched.

2. **`AddOrderPreProcessor` — contra-side marketable test (matching bug).**
   `preProcess()` decided whether an incoming LIMIT order crosses (`AGGRESS_ORDER`)
   or rests (`ADD_ORDER`) by comparing its price against its **own** side's best
   (buy vs `bestBid`, sell vs `bestOffer`). A buy is marketable iff it can hit an
   **ask** (`price ≥ bestOffer`) and a sell iff it can hit a **bid**
   (`price ≤ bestBid`) — the **contra** side. The own-side test mis-routes: in
   particular, when a side is empty its best is 0, the `best != 0` guard is
   skipped, and a non-marketable order falls through to `AGGRESS_ORDER`, while the
   `ADD_AND_AGGRESS` touch branch then adds-then-sweeps in a different order than a
   plain aggress-then-rest — producing a different trade/state stream from the
   price-time baseline. The fix tests the **contra-side** best and routes every
   marketable visible LIMIT order through `AGGRESS_ORDER` (standard price-time:
   aggress, then rest the residual). The `ADD_AND_AGGRESS`/`bestVisible` touch
   branch is dropped — it exists for the hidden/iceberg interaction (`bestVisible`
   differs from `best` only with `HIDDEN_LIMIT` orders), and for plain visible
   LIMIT orders `AGGRESS_ORDER` is the correct, equivalent action; the empty-tree
   short-circuits are kept.

3. **`PriceTimePriorityStrategy` — per-fill observation sink (no matching change).**
   CoinTossX's fill record (`ExecutionReportData.addFillGroup(price, qty)`)
   collapses same-price fills into a map and keeps **no counterparty ids**, so it
   cannot supply the per-fill maker/taker order ids the harness `ME_TRADE` report
   requires. The patch adds a `public static FillSink` and emits one fill
   (`maker = currentOrder`, `taker = aggOrder`, `price`, `quantity` — all already
   in scope) immediately after the engine's existing `addTrade(...)` in
   `processOrdersInList`. It records data already computed by the matcher and
   changes no matching logic, so it can neither hide nor create a matching bug.

(`build.sh` also disables CoinTossX's circuit breaker and runs only the
continuous-trading session via the engine's own public configuration
(`MatchingUtil.setEnableCircuitBreaker(false)`, `TradingSessionFactory`) — that
is helper setup through the engine's API, not a source change.)

## Build / run

```bash
bash additional_references/cointossx_adapter/build.sh
./harness --engine cointossx_adapter.so --scenario normal --mode audit \
          --matcher-core 54 --drainer-core 55
```

`build.sh` clones CoinTossX into `third_party/cointossx_CoinTossX/` at the pinned
commit (no git submodules; the one vendored jar,
`lib/ObjectLayout-1.0.5-SNAPSHOT.jar`, is a tracked file the reset restores),
applies the three patches above, resolves the eight matcher dependency jars
(hppc, aeron-client, aeron-driver, Agrona, sbe, joda-time, HdrHistogram,
commons-csv) into `third_party/cointossx_deps/` (from the local `~/.m2` cache if
present, else by download from Maven Central), compiles the matcher core +
`HarnessCoinTossX` into `third_party/cointossx_build/classes/`, jars it, writes
`cointossx.classpath` at the repo root, and compiles `cointossx_adapter.so` at the
repo root (with the JDK's `libjvm.so` path baked in). All generated output lands
under the gitignored `third_party/` tree; the adapter directory itself holds only
the authored `cointossx_adapter.cpp`, `HarnessCoinTossX.java`, `build.sh`, and this
README. The adapter reads `./cointossx.classpath` at run time and embeds the JVM
via that classpath.

Requires a **JDK 11** (`build.sh` auto-installs `openjdk-11-jdk-headless` via
`apt` only if no JDK 11 is found). JDK 11 is used deliberately: CoinTossX (2015,
Java 8) reads `sun.misc.Unsafe` via `theUnsafe`-field reflection for its off-heap
order nodes, which JDK 11 exposes without `--add-opens`; the engine sources are
compiled `-source 8 -target 8`. Overrides: `ME_CTX_SRC` uses an existing CoinTossX
checkout in place of cloning; `ME_JDK` selects a specific JDK 11; `ME_M2` points
at a local Maven cache to copy the jars from.
