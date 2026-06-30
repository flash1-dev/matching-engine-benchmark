# Resolved findings

Engine-level defects the benchmark surfaced that have since been **fixed
upstream**. Each is moved here out of `CORRECTNESS_FINDINGS.md` — which tracks only
current, open findings — with its original analysis preserved and the resolution
recorded. The harness keeps pinning the pre-fix snapshot of each engine (so its
published figures are unchanged) unless noted otherwise.

## chronoxor/CppTrader — `ModifyOrder` order-index corruption

**Status — RESOLVED upstream (2026-06-18).** Reported as CppTrader
[issue #42](https://github.com/chronoxor/CppTrader/issues/42) and fixed the same
day in commit
[`731ea64`](https://github.com/chronoxor/CppTrader/commit/731ea64674) — the
maintainer's reply was *"Fixed! Thanks for reporting."* The fix is the one-line
re-find this analysis predicted:

```diff
-        _orders.erase(order_it);
+        _orders.erase(_orders.find(order_ptr->Id));
```

applied to `ModifyOrder` and the five sibling erase-after-operation sites that
share the same stale-iterator hazard (`ReduceOrder`, `ReplaceOrder`,
`DeleteOrder`, and both `ExecuteOrder` overloads). On the five standard benchmark
scenarios CppTrader was always clean (VALID ×5); the defect only ever triggered
off the canonical path. The harness still pins the pre-fix snapshot (`831d10e2`),
so CppTrader's published figures are unchanged — bump the pin past `731ea64` to
pick up the fix.

### The finding (as recorded before the fix)

On the canonical workload CppTrader is clean: a byte-identical report stream
against the byte-identical consensus on all five scenarios, with 192/192
state-audit checks matching on each. We record one engine-level defect
anyway, because we hit it while stress-testing during the 2026-06-11
workload re-anchoring and verified its mechanics in a debug build of the
pinned snapshot:

- Under a development stress configuration (a deeper, time-ordered standing
  book — not the shipped workload), a one-tick reprice modify that crossed
  and **filled completely** inside `MarketManager::ModifyOrder`'s re-match
  left its order node in the engine's id index (`_orders`, the map behind
  `GetOrder`) — fully executed (`ExecutedQuantity == Quantity`,
  `LeavesQuantity == 0`) and with its `Level` pointer null (unlinked from
  its price level at the start of the modify and never re-linked, since
  nothing remained to rest).
- A later cancel of that id — which the engine's own index reported as a
  live order — reached `OrderBook::DeleteOrder`, dereferenced the null level
  pointer (`order_book.cpp:199`), and crashed. An adapter that keeps its
  *own* liveness shadow masks the corruption — the stale id is rejected
  adapter-side, the run completes, and the damage surfaces only as
  CppCommon's pool assertion at engine teardown (`"Memory leak detected!
  Allocated memory size must be zero!"` in `PoolMemoryManager::clear`). An
  adapter that treats the engine's `GetOrder` as the liveness oracle — the
  engine's own API for the question — crashes. We verified both behaviours
  against the same engine build.
- The trigger is **narrower than "any fully-filled crossing modify"**: the
  canonical `normal` realisation contains 39 crossing modifies that fill
  completely, 38 of them later cancelled, and none trips the defect.
- **Root cause — a stale hash-map handle reused across the re-match.**
  `_orders` is a `CppCommon::HashMap`: open addressing with *backward-shift*
  deletion, so erasing one key can relocate *other* live keys to earlier
  buckets. `MarketManager::ModifyOrder` caches the order's `find` iterator
  (`market_manager.cpp:578`) and reuses it to erase the order *after* the
  re-match (`:664`). But the re-match (`:631` `MatchLimit`) erases every maker
  it fully consumes — `ReduceOrder` → `_orders.erase` (`:541`) — and each such
  erase can shift the aggressor's own bucket. When it does, the `:664` erase
  fires on the now-stale bucket index: it blanks the wrong slot and leaves the
  fully-filled aggressor in `_orders` with a null `Level`. The engine's own
  stop-activation paths re-find by id immediately before erasing
  (`:1407`/`:1445`, `_orders.erase(_orders.find(Id))`); `ModifyOrder` is the
  lone post-match erase that trusts the cached handle. New-order insertion
  (it matches first, inserts only what rests) and `ReplaceOrder` (it erases
  before matching) are both immune — the defect is `ModifyOrder`-only, and a
  one-line fix (erase by id / a fresh `find` at `:664`, as the stop paths do)
  closes it. With `std::unordered_map`'s node stability the original code is
  correct, which is likely why it shipped.
- **Why it is load-dependent.** The relocation happens only when the aggressor's
  bucket collides into a consumed maker's probe run — a hash- and
  load-geometry property: rare at the canonical `normal` standing book (the 39
  fully-filled crossing modifies all escape), increasingly likely as the book
  deepens. An instrumented debug build of the pinned snapshot reproduces it
  deterministically: at a few thousand resting orders, a fully-filling crossing
  modify relocates the aggressor's bucket mid-match (verified in the map's
  backward-shift), the stale erase blanks the wrong slot, and a subsequent
  cancel null-derefs at `order_book.cpp:199`.

The upstream fix (above) is exactly this one-line re-find, generalized by the
maintainer to every erase site that could outlive a relocation.

### CppTrader integration notes

Two operational details worth noting for an integrator (unaffected by the fix):

- `MarketManager::EnableMatching()` is **OFF by default**. Without it the
  engine silently rests every order without crossing — every aggressor would
  rest with full unfilled quantity and produce zero Trade reports. The
  reference adapter enables matching once after the order book is created.
- `MarketHandler::onExecuteOrder` fires **twice per fill**, first with the
  maker (resting) order, then with the taker (incoming). The reference
  adapter pairs consecutive callbacks into one harness Trade report and
  tallies the taker's filled quantity for IOC residual accounting.

## geseq/orderbook — multi-level crossings ignore the price predicate

**Status — RESOLVED upstream (2026-06-20).** Reported as geseq/orderbook
[issue #25](https://github.com/geseq/orderbook/issues/25) and fixed the same day
in commit
[`8bf1381`](https://github.com/geseq/orderbook/commit/8bf1381ec90e) — the
maintainer's reply was *"Thanks for the report. I'll patch this shortly."* The
fix is the one-line predicate re-check this analysis predicted: the
`processLimitOrder` execution loop now re-applies the cross predicate on every
iteration instead of only at the entry guard, so a marketable order stops at its
own limit rather than consuming the next-best level. The maintainer also added a
CI correctness gate that runs this benchmark against the engine
([`ba3a635`](https://github.com/geseq/orderbook/commit/ba3a635425eb)). The
harness pins a later upstream commit
([`88e8098`](https://github.com/geseq/orderbook/commit/88e80980c691)) that
carries the fix, so geseq is VALID on all five
scenarios with no adapter patch; its published figure is unchanged, since the
upstream fix is the same one-line predicate the adapter previously applied. The
pre-fix snapshot it carried was `3b9e9cd`.

The same defect, and the same fix, appeared in the author's C++ port
[geseq/cpp-orderbook](https://github.com/geseq/cpp-orderbook); there the fix was
already merged upstream before we integrated it, so that engine's pinned commit
contains it and its adapter needs no patch (see `CORRECTNESS_FINDINGS.md`).

### The finding (as recorded before the fix)

In `pricelevel.go::processLimitOrder`, the inner matching loop iterates
across queues but does not re-check the price-cross predicate inside the
loop:

```go
// shipped form — predicate checked only once before entering
for orderQueue := pl.GetQueue();
    qtyLeft.GreaterThan(decimal.Zero) && orderQueue != nil;
    orderQueue = pl.GetQueue() {
    _, q := orderQueue.process(ob, takerOrderID, qtyLeft)
    qtyLeft = qtyLeft.Sub(q)
    qtyProcessed = qtyProcessed.Add(q)
}
```

Once the best queue crosses, the loop continues consuming subsequent queues
regardless of whether their price still crosses the aggressor's limit.
Concretely: a sell IOC at $32.86 that fills the maker at $32.96 (correctly)
keeps consuming the next-best maker at $32.74 (incorrectly).

How it presents in the harness:

- Without a source patch, the report-stream hash FAILs on all five
  scenarios and the state audit mismatches on most.
- With a one-line patch that adds `&& compare(orderQueue.Price())` to the
  loop header, all five scenarios PASS and state audit returns 192/192 on
  every scenario.

The reference adapter applied the patch as a build step (same pattern as
jxm35's `notify_trade` patch) until the fix landed upstream. We did not
investigate whether the original
loop was an oversight or a deliberate simplification — the upstream README
does not mention multi-level crossing semantics either way. A downstream
consumer of fills running the un-patched engine would see fills at prices
that should not have crossed.

## GOnevo/matchingo — price-level volume not conserved after a partial fill

**Status — RESOLVED upstream (2026-06-22).** Reported as GOnevo/matchingo
[issue #1](https://github.com/GOnevo/matchingo/issues/1) and fixed the same day —
the maintainer's reply was *"@OPTIONPOOL fixed — thanx for bug report!!!"* and the
issue was closed as completed. The harness still pins the pre-fix snapshot
(`7aa642f`, `v0.0.1`), so matchingo's published figure is unchanged; its reference
adapter applies the same one-line write-back fix until the pin is bumped past the
upstream release.

### The finding (as recorded before the fix)

matchingo's *matching* is correct — its trade/report stream is byte-identical to
the consensus on all five scenarios. But the harness's 192-point depth audit
fails: a price level's `volume` stops being conserved the moment an incoming order
partially fills a resting order, and can even go negative.

`OrderQueue.UpdateVolume` (`orderqueue.go:51-53`, `v0.0.1` / `7aa642f`) subtracts
the resting order's *remaining* quantity (`o.Quantity`) instead of the *consumed*
quantity. On a full fill the two coincide and the cache stays correct; on a
partial fill they differ, so the level's cached `volume` drifts (and can go
negative) even though the emitted trades are right — exactly what a
report-stream-only check misses and the state audit catches. The fix subtracts
the consumed quantity; the reference adapter applied it as a build step (same
pattern as jxm35's `notify_trade` patch) until the upstream release landed.

## CheetahExchange/orderbook-rs — `cancel_order` searches the opposite side's book

**Status — RESOLVED upstream (2026-06-23).** Reported as
CheetahExchange/orderbook-rs
[issue #1](https://github.com/CheetahExchange/orderbook-rs/issues/1); the
maintainer replied *"an obvious flaw … I'll release a new commit to fix it
immediately"* and closed the issue as completed. The harness pins the pre-fix
snapshot (`caa33f3`), so this engine (rostered as `cheetah`) stays non-conforming
on the pinned commit; bumping the pin past the upstream fix restores cancel
handling.

### The finding (as recorded before the fix)

orderbook-rs diverged from the consensus on all five scenarios. The cause is in
the pure matcher (`src/matching/order_book.rs`, `caa33f3`), reachable directly
through `OrderBook::{apply_order, cancel_order}` — the exact pair the engine's
`run_applier` drives per order.

A resting order is filed in its own side's book, but **`cancel_order` looked it up
in the *opposite* side's book**, so no resting order could ever be cancelled:
every cancel failed and the order leaked, diverging both the report stream and the
book state from the consensus. We also flagged, as a suggestion, that the engine's
10k-id de-dup `Window` assumes monotonically-increasing order ids and silently
drops non-monotonic ones (~97% on the benchmark's id pattern); the maintainer
confirmed this stems from an auto-incrementing-id assumption (a snowflake-style id
would need a different structure) — a design assumption rather than a matcher bug,
recorded for completeness.

## joaquinbejar/OrderBook-rs — partial fill demotes the resting maker to the FIFO tail

**Status — RESOLVED upstream (2026-06-24).** Reported as OrderBook-rs
[issue #88](https://github.com/joaquinbejar/OrderBook-rs/issues/88); the maintainer
confirmed and reproduced it at the cited locations, traced the root cause to the
upstream `pricelevel` crate, and fixed it: *"`pricelevel 0.8.0` shipped with the
queue-priority fix (PriceLevel#39), and #131 bumps the dependency (`pricelevel`
0.7 → 0.8.0) and adds a regression test on the matching path."* The harness pins the
pre-fix snapshot, so the engine's published figure is unchanged; its reference
adapter carried the equivalent fix until the dependency bump landed.

### The finding (as recorded before the fix)

A partial fill sent the resting maker to the back of its own price-level FIFO queue
instead of leaving it at the front, so the next incoming order matched the wrong
counterparty (a later same-price order) while the genuinely-oldest order waited.
Quantities and the trade total stayed correct — only counterparty selection
(time priority within a level) diverged from the consensus, VALID ×5 across 100
seeds once corrected. The defect lived in the `pricelevel` dependency's in-level
order container, which OrderBook-rs delegates to.

## fran0x/matchina — phantom zero-quantity trades (no taker-exhaustion guard)

**Status — RESOLVED upstream (2026-06-23).** Reported as matchina
[issue #3](https://github.com/fran0x/matchina/issues/3); the maintainer confirmed —
*"You were spot on: the inner loop kept going after the taker was fully filled,
which caused the zero-quantity phantom trades. I've fixed it and added a [test]."*
The harness pins the pre-fix snapshot; the reference adapter carried the one-line
guard until the fix landed.

### The finding (as recorded before the fix)

The price-level matching loop had no taker-exhaustion guard: after an incoming order
was fully filled, the loop kept iterating over remaining resting orders and emitted
additional trades for zero quantity. Matching was otherwise correct; the phantom
zero-qty trades diverged the report stream from the consensus, VALID ×5 across 100
seeds with the guard added.

## film42/rinok — buy-side maker mispricing

**Status — RESOLVED upstream (2026-06-28).** Reported as rinok
[issue #2](https://github.com/film42/rinok/issues/2); the maintainer confirmed and
fixed it — *"I think you nailed the issue. I added more tests and this is now
passing."* The harness pins the pre-fix snapshot; rinok is rostered EPL-1.0 (glue
shipped, engine fetched at build) and is unchanged on the pinned commit.

### The finding (as recorded before the fix)

A buy order crossing a lower-priced resting sell printed the trade at the incoming
buy's price rather than the resting maker's, so the aggressor lost the price
improvement it was due (buy-initiated crossings only; the sell-initiated path was
correct). Book state stayed consistent; only the trade price field diverged.

## yihuang/pyorderbook — `cancel_order` KeyError on an emptied price level

**Status — RESOLVED upstream (2026-06-28).** Reported as pyorderbook
[issue #1](https://github.com/yihuang/pyorderbook/issues/1); the maintainer fixed it
(*"thanks for reporting"*, closed as completed via commit
[`46da454`](https://github.com/yihuang/pyorderbook/commit/46da4543b1)). The harness
pins the pre-fix snapshot.

### The finding (as recorded before the fix)

`cancel_order` indexed `self.levels[price]` without guarding for a level that a prior
fill had already emptied and removed, so a too-late cancel of an already-consumed
order raised an unhandled `KeyError` and aborted the run instead of rejecting the
cancel. Matching on the canonical workload was otherwise consensus-correct (the
engine is rostered as a Wave-8 conformer).

## dx1ngy/trading — fills priced at the sell order's price, not the maker's

**Status — RESOLVED upstream (2026-06-29).** Reported as dx1ngy/trading
[issue #1](https://github.com/dx1ngy/trading/issues/1); the maintainer confirmed —
*"the reproduction, root-cause analysis, and diff are all clear, and the issue is
confirmed"* — and closed it via commit
[`b1f835d`](https://github.com/dx1ngy/trading/commit/b1f835dbfd). The harness pins
the pre-fix snapshot; the reference adapter carried the maker-price fix
(load-bearing for conformance) until the fix landed.

### The finding (as recorded before the fix)

`match()` priced every fill at the sell order's price, so a sell crossing a higher
resting bid printed at the aggressor's lower price instead of the resting maker's —
a price-time-priority (maker-price) violation. Matching, FIFO, and quantities were
otherwise consensus-correct (Wave-9 conformer), VALID with the one-line maker-price
correction.
