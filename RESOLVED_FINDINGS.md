# Resolved findings

Engine-level defects the benchmark surfaced that have since been **fixed
upstream**. Each is moved here out of `discoveries.md` — which tracks only
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
against the three-baseline consensus on all five scenarios, with 192/192
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
