# coralme_adapter — integration example

Wraps [coralblocks/CoralME](https://github.com/coralblocks/CoralME) behind
`api/matching_engine_api.h`. CoralME is a garbage-free Java matching engine; the
harness is native, so this adapter embeds a JVM (JNI) and drives a CoralME
`OrderBook` through a small Java helper (`HarnessCoralMe`).

Pinned commit:
- `coralblocks/CoralME` — `6d0f94898f05ca7059a79551132be16c17785863`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot (CoralME is
**conforming as shipped** — no source fix required).

## Engine shape

`com.coralblocks.coralme.OrderBook` is the matcher: a price-ordered doubly-linked
list of `PriceLevel`s, each level a FIFO order list (time priority), plus a
`LongMap<Order>` id index — all backed by CoralME's object pools (CoralPool +
CoralDS), so steady-state matching is allocation-free. Native APIs the adapter
uses:

- `OrderBook.createLimit(clientId, clientOrderId, exchangeOrderId, Side, size,
  long price, TimeInForce)` — submits a limit order under the caller-chosen
  `exchangeOrderId`; the engine's own price-time matcher crosses it against the
  best contra levels and rests the residual. The `long price` overload takes the
  harness's signed integer ticks directly as CoralME's opaque comparable price.
- `OrderBook.getOrder(long id)` — the engine's own id index; returns the resting
  `Order` or `null`.
- `Order.cancel(CancelReason)` — the engine's own removal (drives the listener
  chain). `Order.isResting()` / `getPrice()` / `getSide()` / `getOpenSize()` /
  `getId()` read resting state.
- `OrderBook.hasBids` / `getBestBidPrice`, `hasAsks` / `getBestAskPrice`, and
  `getOrders()` (the id-index `LongMap`) for the audit queries.
- `OrderBookListener` (engine-shipped interface) — `onOrderExecuted(book, time,
  Order, ExecuteSide, execSize, execPrice, execId, matchId)` fires **twice per
  fill**, once `MAKER` and once `TAKER`, both carrying the maker's resting
  `execPrice`. `TimeInForce.IOC` / `GTC` cover the two order types the harness
  needs.

Not provided natively: no native modify (CoralME has `cancel` + re-add, not an
in-place amend); `ExecuteSide`/`onOrderExecuted` carries no taker id (the maker
leg's `order.getId()` is the maker; the taker is the order currently
aggressing); orders are named by the caller's `long` id (this maps the harness
`uint64_t` id straight through).

## Adapter strategy

- **Order ids**: the harness's dense `uint64_t` id is passed straight to
  CoralME as the `exchangeOrderId`, so `cancel`/`modify` find the `Order` by id
  through the engine's own `getOrder` index and the maker leg of each execution
  yields the maker id directly. No id translation map is needed.
- **Reports**: `HarnessCoralMe` registers itself as the book's
  `OrderBookListener` and, on each matching call, captures **only the `MAKER`
  leg** of every `onOrderExecuted` (the maker leg gives the maker id and the
  resting fill price; the taker is the order currently aggressing, tracked as
  `curTakerId`) into an adapter-owned direct `ByteBuffer` (the single matcher
  thread is the only writer). The native side turns each staged fill into a
  `ME_TRADE` and adds the `OrderAck` / `CancelAck` / `ModifyAck` (and
  `CancelReject` / `ModifyReject`) reports, pushing all of them through the
  harness transport. There is no per-order price/side shadow: the maker price is
  read off the execution itself, and a cancel reads the live `Order`'s price/side
  from the engine before removing it — no adapter-side book state.
- **Prices**: the harness's signed integer ticks are written straight into
  CoralME's `long`-price `createLimit` overload (CoralME treats the price as an
  opaque comparable long), so tick ordering is preserved exactly.
- **Single client / self-trade**: every order uses one `clientId` and one
  fixed non-empty `clientOrderId`. CoralME's `OrderBook(security)` ctor defaults
  `allowTradeToSelf = true`, so one client id for all orders does not change the
  matching or the trade output — exactly as the exchange-core baseline uses one
  uid.
- **IOC**: submitted as `TimeInForce.IOC`, so CoralME matches it and drops any
  residual itself; the adapter emits the residual `CancelAck` when the filled
  quantity is short of the order size.
- **Modify**: cancel + reinsert (the harness rule) — `getOrder` the resting
  order, `cancel` it (no trade, no report), then re-add at the new price/qty with
  fresh time priority via the same `createLimit` path, emitting each crossing
  fill plus one `ModifyAck`, or a `ModifyReject` if the order was not resting.
- **Cancel adjudication**: `getOrder(id)` + `isResting()` is the resting test —
  a hit is cancelled and acked (price/side echoed from the live `Order`), a miss
  (already filled / already cancelled / never seen) is a `CancelReject`.
- **Depth query**: `depthAt` walks the engine's own `getOrders()` id index
  summing the open size at `(price, side)`. It is a sparse audit query, never on
  the hot path, so a public-API walk is fine and keeps the engine unpatched.
- **JVM**: `engine_init` (on the harness matcher thread) `dlopen`s `libjvm.so`
  with `RTLD_NODELETE` (so the harness's final `dlclose` can't unload a live
  JVM), creates the VM with SerialGC and a pre-touched fixed 2 GiB heap (removes
  GC / heap-resize / page-fault noise from the measured pass), constructs
  `HarnessCoralMe`, hands it the staging buffer, and warms the JIT on a
  throwaway book that is then discarded. Every `engine_*` call runs on that same
  thread, so one cached `JNIEnv` is valid throughout and `engine_flush` is a
  no-op (CoralME matches synchronously). No `engine_prebuild` is exported.
- **Batch path** (optional): `engine_on_batch` crosses into the JVM once per
  chunk — `HarnessCoralMe.onBatch` matches every message and writes the full
  `me_report_t` stream into a second direct buffer that the native side drains to
  the transport, amortizing the per-message JNI crossing the way the Go adapters
  amortize cgo. It mirrors the per-message path exactly.

## Source patch

**No source patch.** CoralME is conforming as shipped; `build.sh` clones the
engine and `git reset --hard`s it to the pinned commit without modifying any
engine file (the working tree stays pristine apart from Maven's own `target/`
build output). All harness glue lives in `coralme_adapter.cpp` and
`HarnessCoralMe.java`.

The only **build** consideration (not an engine-source change) is dependency
resolution: CoralME depends on two of the author's own libraries, `CoralPool`
and `CoralDS`, which it publishes through [JitPack](https://jitpack.io) (declared
in CoralME's own `pom.xml`). `build.sh` runs the project's `maven-shade-plugin`
to produce the self-contained `coralme-all.jar`, which bundles those two deps;
Maven resolves them from JitPack (or the local `~/.m2` cache if already present).
No engine logic is involved.

## Build / run

```bash
bash additional_references/coralme_adapter/build.sh
./harness --engine coralme_adapter.so --scenario normal --mode audit \
          --matcher-core 56 --drainer-core 57
```

`build.sh` clones CoralME into `third_party/coralme_CoralME/` at the pinned
commit, builds the shaded `coralme-all.jar` with Maven (`-DskipTests`,
javadoc/sources skipped), compiles `HarnessCoralMe.java` into
`third_party/coralme_build/classes/`, writes `coralme.classpath` (the shaded jar
+ the helper class dir) at the repo root, and compiles `coralme_adapter.so` at
the repo root (with the JDK's `libjvm.so` path baked in). All generated output
lands under the gitignored `third_party/` tree; the adapter directory itself
holds only the authored `coralme_adapter.cpp`, `HarnessCoralMe.java`, `build.sh`,
and this README. The adapter reads `./coralme.classpath` at run time and embeds
the JVM via that classpath.

Requires a JDK 21 (`build.sh` auto-installs `openjdk-21-jdk-headless` via `apt`
only if no JDK is found) and Maven on `PATH`. Overrides: `ME_CORALME_SRC` uses an
existing CoralME checkout in place of cloning; `ME_JDK` selects a specific JDK.
The adapter's `libjvm.so` path is overridable at run time with `ME_JVM_LIB`.
