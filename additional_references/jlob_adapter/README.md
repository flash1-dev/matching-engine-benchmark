# jlob_adapter — integration example

Wraps [eliquinox/jLOB](https://github.com/eliquinox/jLOB) behind
`api/matching_engine_api.h`. jLOB is a Java L3 limit order book; the harness is
native, so this adapter embeds a JVM (JNI) and drives jLOB's matcher through a
small Java helper (`HarnessJlob`).

Pinned commit:
- `eliquinox/jLOB` — `c78c2a2ce77c339b2343a1678f881fc9749fbd87`

This adapter is one of the worked examples in `additional_references/` — none are
baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the repository
root for the observations the harness produced against this snapshot (jLOB is
**conforming as shipped** — no source fix required).

## Engine shape

`state.LimitOrderBook` is the matcher: bids and offers are price-sorted fastutil
`Long2ObjectRBTreeMap`s of `Limit` price levels, each level a FIFO `ArrayList`
of `Placement`s, with a UUID→`Placement` index. Native APIs the adapter uses:

- `LimitOrderBook.place(Placement)` — the engine's own price-time matcher
  (aggress against the best contra levels, then rest the residual).
- `LimitOrderBook.cancel(Cancellation)` — the engine's own removal (throws
  `JLOBException` on an unknown/oversized cancel).
- `LimitOrderBook.getBestBid` / `getBestOffer`, plus `streamBids` / `streamOffers`
  (which yield `Limit`s whose `getVolume()` is the resting depth) for the audit
  queries.
- `state.LimitOrderBookListener` (engine-shipped interface) — `onMatch(Match)`
  fires once per crossing fill; `Match` carries the maker/taker `Placement` UUIDs
  and the fill size. `onPlacement` / `onCancellation` also fire (unused here).

Not provided natively: no IOC / FOK / POST-ONLY order type; no native modify;
orders are identified by `UUID` (the harness uses dense `uint64_t` ids); `Match`
carries no maker price; `cancel` signals "unknown order" by throwing, not by a
return code.

## Adapter strategy

- **Order ids**: the harness id is mapped to a deterministic
  `UUID(0, order_id)`, so `cancel`/`modify` find the `Placement` by id and a
  `Match`'s maker/taker UUID recovers the harness id from its low 64 bits.
  `Placement` mints a random UUID in its ctor and `withUuid` is private, so the
  helper overwrites the (final) `uuid` field through a once-unlocked reflective
  `Field` handle — adapter glue for choosing the id, not a change to matching.
- **Reports**: `HarnessJlob` registers itself as the book's
  `LimitOrderBookListener` and writes one fill record per `onMatch` into an
  adapter-owned direct `ByteBuffer` (the single matcher thread is the only
  writer); the native side turns each into a `ME_TRADE` and adds the
  `OrderAck` / `CancelAck` / `ModifyAck` (and `CancelReject` / `ModifyReject`)
  reports, pushing all of them through the harness transport.
- **Maker price**: `Match` omits it, so the helper keeps a per-order
  liveness/price/side shadow (`order_id → {price, side, remaining}`, a fastutil
  map) and reads the maker's resting price off it for the trade record. This is
  the same minimal state the C++ reference adapters keep for the same reasons;
  it never matches and never drives priority. A partial reduction is tracked so
  a later cancel of the same id still reports the right resting price/side.
- **IOC**: jLOB has no IOC type, so an IOC new order is matched by `place()` and
  any residual is removed via the engine's own `cancel`; the adapter emits the
  residual `CancelAck`.
- **Modify**: cancel + reinsert (the harness rule) — remove the resting order
  via the engine's `cancel`, then re-add at the new price/qty with fresh time
  priority, emitting each crossing fill plus one `ModifyAck`, or a
  `ModifyReject` if the order was not resting.
- **Cancel adjudication**: the shadow's presence is the resting test — a hit is
  acked (price/side echoed from the shadow), a miss (already filled / already
  cancelled / never seen) is a `CancelReject`. The engine's `cancel` throw is
  caught as a backstop.
- **JVM**: `engine_init` (on the harness matcher thread) `dlopen`s `libjvm.so`,
  creates the VM with SerialGC and a pre-touched fixed 2 GiB heap (removes
  GC/heap-resize/page-fault noise from the measured pass), constructs
  `HarnessJlob`, and warms the JIT on a throwaway book that is then discarded.
  Every `engine_*` call runs on that same thread, so one cached `JNIEnv` is
  valid throughout and `engine_flush` is a no-op (jLOB matches synchronously).
  No `engine_prebuild` is exported.

## Source patch

**No source patch.** jLOB is conforming as shipped; `build.sh` clones the engine
and `git reset --hard`s it to the pinned commit without modifying any engine
file.

Two pieces of **build scaffolding** (not engine-source changes) let the adapter
compile and run jLOB's matcher in isolation from its datastore — both are
documented here for completeness:

1. **Matcher-subset compile** — the full `gradle build` stands up a live
   PostgreSQL + Redis and runs jOOQ code generation; none of that is the
   matcher. `build.sh` compiles only the nine self-contained matcher classes
   (`state.LimitOrderBook`/`Limit`/`LimitOrderBookListener`/
   `DummyLimitOrderBookListener`, `dto.Placement`/`Match`/`Cancellation`/`Side`,
   `exceptions.JLOBException`) plus `HarnessJlob`, against the five jars the
   matcher actually needs (fastutil, guice, javax.inject, guava, commons-lang3).
2. **No-op `cache.Cache` compile stub** — `state.LimitOrderBook` imports
   `cache.Cache` only from a *public* constructor the adapter never calls (the
   helper builds the book through jLOB's own *private*
   `LimitOrderBook(LimitOrderBookListener)` ctor by reflection, to install its
   own listener). jLOB's real `cache.Cache` is Redisson-coupled (it
   `import`s `org.redisson.*` and `config.RedisConfig` and opens a
   `RedissonClient` in its ctor), so compiling it would drag in the whole Redis
   stack. `build.sh` instead generates a three-method no-op `cache.Cache` into
   the gitignored `third_party/jlob_build/classes-src/` and passes *that* file to
   `javac` (the engine's real `cache/Cache.java` is left out of the compile set)
   — **the stub is never written into the cloned engine tree**, which stays
   byte-for-byte pristine (`git status` clean). It exists only to satisfy the
   compile-time `import cache.Cache` in `LimitOrderBook`; the Redis-backed ctor
   is never invoked, so Redisson is never loaded at run time. No matching logic
   is touched.

## Build / run

```bash
bash additional_references/jlob_adapter/build.sh
./harness --engine jlob_adapter.so --scenario normal --mode audit \
          --matcher-core 58 --drainer-core 59
```

`build.sh` clones jLOB into `third_party/jlob_jLOB/` at the pinned commit,
resolves the five dependency jars into `third_party/jlob_deps/` (from the local
`~/.m2` cache if present, else by download from Maven Central), compiles the
matcher subset + `HarnessJlob` into `third_party/jlob_build/classes/`, writes
`jlob.classpath` at the repo root, and compiles `jlob_adapter.so` at the repo
root (with the JDK's `libjvm.so` path baked in). All generated output lands
under the gitignored `third_party/` tree; the adapter directory itself holds
only the authored `jlob_adapter.cpp`, `HarnessJlob.java`, `build.sh`, and this
README. The adapter reads `./jlob.classpath` at run time and embeds the JVM via
that classpath.

Requires a JDK 21 (`build.sh` auto-installs `openjdk-21-jdk-headless` via `apt`
only if no JDK is found). Overrides: `ME_JLOB_SRC` uses an existing jLOB checkout
in place of cloning; `ME_JDK` selects a specific JDK; `ME_M2` points at a local
Maven cache to copy the jars from.
