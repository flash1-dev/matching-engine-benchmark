# trademacher_adapter — integration example

Wraps [TradeMatcher/match-engine](https://github.com/TradeMatcher/match-engine)
(Maven artifact `match-engine-core`, package `com.tradematcher`) behind
`api/matching_engine_api.h`. TradeMatcher is a Java price-time-priority matching
engine; the harness is native, so this adapter embeds a JVM (JNI) and drives
TradeMatcher's *matcher* — without the LMAX-Disruptor command pipeline,
WebSocket, or journal in the loop — through a small Java helper
(`HarnessTradeMacher`).

Pinned commit:
- `TradeMatcher/match-engine` — `552c71a83f0d28808048189a1153a6463ea661ef`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot (TradeMatcher
is **conforming as shipped** — no source fix required).

## Engine shape

`com.tradematcher.entity.OrderBookImpl` is the matcher: a per-symbol order book
holding a `TreeMap` of price "buckets" (reverse-ordered for bids), a single
cross-bucket doubly-linked order list per side carrying time priority, and a
`HashMap<String,Order>` id index. The helper drives that core directly — exactly
as the engine's own `GTCTest` / `FAKTest` do (they call into `OrderBook`
methods), with no Disruptor, WebSocket, journal, or snapshot in the loop. Native
APIs the adapter uses:

- `OrderBookImpl.matchByUnitPriceAndSize(id, price, size, action)` — the engine's
  own limit-price + size price-time matcher (used for both a harness new order and
  the cross step of a harness IOC / modify). It returns a `MatchResult` whose
  `Event` chain lists one node per fill.
- `OrderBook.createOrder(price, size, action, id, ...)` — the engine's own
  rest-the-residual path (exactly what `PlaceGTCOrder.action()` does after the
  match).
- `OrderBook.getOrderByID(id)` — the engine's own id index, for the cancel /
  modify existence test and the resting order's price + side.
- `OrderBookImpl.cancelOrder(order)` — the engine's own removal path.
- `OrderBook.getMarketOrderBook(depth).getMarket()` — the engine's own order-book
  snapshot (best-first bid/ask prices + aggregated sizes), for the best-bid /
  best-ask / depth audit queries.

Prices and sizes are `java.math.BigInteger` throughout the engine; `action` is
the engine's `Constants.Action` (`BID` = buy, `ASK` = sell), and the engine's
fill events are tagged `MAKER` / `TAKER` (`Constants.EventType`).

Not provided natively in a form the harness uses directly: orders are identified
by `String` ids (the harness uses dense `uint64_t`); the engine's own FAK uses
total-price / wanted-size semantics (a different order kind from the harness's
limit-price + size IOC); there is no native modify; and the per-fill `Event`
carries the maker leg (resting order id, resting unit price, filled size) but not
the aggressor id.

## Adapter strategy

- **Order ids**: the harness `uint64_t` id is mapped to its decimal string
  (`Long.toString` / `Long.parseLong`) — the engine names orders with `String`
  ids, so this is the minimal translation; no adapter-side order/book state is
  kept (the engine's own `getOrderByID` is the resting test).
- **Reports**: the engine's matcher produces fills; `HarnessTradeMacher` walks the
  `MatchResult`'s `MAKER` `Event` chain and writes one fill record per maker leg
  (aggressor seq, maker resting price, leg size, maker id, taker id) into an
  adapter-owned direct `ByteBuffer` (the single matcher thread is the only
  writer). The native side turns each into a `ME_TRADE` and adds the `OrderAck` /
  `CancelAck` / `ModifyAck` (and `CancelReject` / `ModifyReject`) reports, pushing
  all of them through the harness transport.
- **Maker price**: it comes straight off the engine's own `Event`
  (`event.getUnitPrice()`), so no maker-price shadow is needed — TradeMatcher's
  fill record already carries the resting maker price, unlike the CoinTossX / jLOB
  references.
- **NEW = GTC limit**: `matchByUnitPriceAndSize` matches the marketable part, then
  the residual is rested via `createOrder` — the engine's own
  `PlaceGTCOrder` sequence.
- **IOC**: TradeMatcher's own FAK is a different (total-price/wanted-size) order
  kind, so a harness limit-price IOC reuses the GTC matcher
  (`matchByUnitPriceAndSize`) and simply does not rest the residual; the native
  side emits the IOC-residual `CancelAck` when `filled < quantity`. This re-uses
  the engine's matcher and re-implements no matching.
- **Modify**: cancel + reinsert (the harness rule) — remove the resting order via
  the engine's own `cancelOrder`, then re-add at the new price/qty with fresh
  (later) time priority via the GTC path, emitting each crossing fill plus one
  `ModifyAck`, or a `ModifyReject` if the order was not resting.
- **Cancel adjudication**: `getOrderByID` is the resting test — a hit is removed
  via `cancelOrder` and acked (the order's own price + side echoed, with the price
  staged in fill record 0 for the native side to read), a miss (already filled /
  already cancelled / never seen) is a `CancelReject`.
- **Audit queries**: best-bid / best-ask / depth-at read the engine's own
  `getMarketOrderBook` snapshot (a sparse, off-hot-path probe), keeping the engine
  unpatched.
- **JVM**: `engine_init` (on the harness matcher thread) `dlopen`s `libjvm.so`
  (`RTLD_NODELETE`, so the harness's final `dlclose` cannot unload a live JVM),
  creates the VM with SerialGC and a pre-touched fixed 2 GiB heap (removes
  GC/heap-resize/page-fault noise from the measured pass), constructs
  `HarnessTradeMacher`, hands it the staging `ByteBuffer`, and warms the JIT on a
  throwaway book that is then discarded. Every `engine_*` call runs on that same
  thread, so one cached `JNIEnv` is valid throughout and `engine_flush` is a no-op
  (TradeMatcher matches synchronously on the calling thread). No `engine_prebuild`
  is exported. The JVM's own service threads (GC, JIT) are created during
  `engine_init`; an engine is free to use threads, so they are not policed.

## Source patch

**No source patch.** TradeMatcher is conforming as shipped; `build.sh` clones the
engine and `git reset --hard`s it to the pinned commit, and all harness glue
(per-message API, report derivation, modify = cancel + reinsert) lives in the
adapter (`trademacher_adapter.cpp`) and the helper (`HarnessTradeMacher.java`) —
no engine `.java` file is edited.

One **build side-effect**, not an engine-source change, is documented for
completeness: the Maven shade plugin regenerates `dependency-reduced-pom.xml` in
the engine root during `mvn package`. Because `build.sh` runs `git reset --hard
<pin>` at the top of every build, the cloned tree is restored to the pristine
pin *before* anything is built each run, so the build is idempotent and the file
is regenerated identically; it is a generated build artifact, not a patch to any
matching logic.

## Build / run

```bash
bash additional_references/trademacher_adapter/build.sh
./harness --engine trademacher_adapter.so --scenario normal --mode audit \
          --matcher-core 52 --drainer-core 53
```

`build.sh` clones TradeMatcher into `third_party/trademacher_match_engine/` at the
pinned commit, builds its self-contained shaded jar with Maven
(`target/match-engine-core-1.0-SNAPSHOT.jar`, which the maven-shade plugin bundles
with all deps — LMAX Disruptor, gson, logback), compiles `HarnessTradeMacher.java`
into `third_party/trademacher_build/classes/`, writes `trademacher.classpath` at
the repo root, and compiles `trademacher_adapter.so` at the repo root (with the
JDK's `libjvm.so` path baked in). All generated output lands under the gitignored
`third_party/` tree; the adapter directory itself holds only the authored
`trademacher_adapter.cpp`, `HarnessTradeMacher.java`, `build.sh`, and this README.
The adapter reads `./trademacher.classpath` at run time and embeds the JVM via
that classpath.

Requires a **JDK 17** and Maven (`mvn`). JDK 17 is used deliberately: the engine
pom targets bytecode 17 and pins Lombok 1.18.26, which does not understand the
JDK 21 compiler internals — JDK 17 is what the engine's own Dockerfile uses
(`maven:3.8.5-openjdk-17`). `build.sh` auto-installs `openjdk-17-jdk-headless` via
`apt` only if no JDK 17 is found. Overrides: `ME_TM_SRC` uses an existing
match-engine checkout in place of cloning; `ME_JDK17` selects a specific JDK 17.
