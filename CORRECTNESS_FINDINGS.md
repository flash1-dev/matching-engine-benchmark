# Correctness findings

This document is the harness's **correctness record**: for each of the **247 engines** driven
through the harness, whether its output conforms to price-time priority, the one-line mechanism
of any divergence, and the upstream issue filed for it. The conformance vocabulary
(**as shipped** / **with fix**, including *latent* defects) is defined in
[`CONSENSUS_CONFORMING_ENGINES.md`](CONSENSUS_CONFORMING_ENGINES.md); the pre-run gate in
[`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

Wherever the harness surfaced a reportable finding we drafted a fix and reported it respectfully — mechanism, reproduction, suggested patch: **181 GitHub issues are filed upstream**, together reporting **more than 250 distinct findings** (a single issue sometimes bundles up to five). **18 of those filed issues are already fixed** by the maintainers, and none so far were declined, marked *wontfix*, or closed as not-planned — [`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md); a few findings were unfileable (repo archived / issues disabled). The per-engine status is:

| | engines |
|:--|--:|
| conform only after a documented matching-correctness fix (incl. *latent* defects) | **110** |
| do not reproduce the consensus (diverge / infeasible / crash) | **87** |
| conform as shipped, surfaced defect since resolved upstream | **3** |
| conform as shipped, no correctness defect (43 fully clean, 4 with a perf/build/off-scope note) | **47** |

**We count findings, not engines: we publish no ranking or count of "buggy" engines.** Non-conforming is not the
same as defective: an engine may fail to reproduce the consensus because it implements a
deliberate design choice — a documented alternative pricing convention, an on-chain per-call gas
cap, a restricted price domain — or because it is byte-identical wherever it completes and is
simply too slow to finish its worst scenario within the message budget. Whether a given
representational limit is a defect or a design decision is a judgment we decline to make on an
author's behalf. 

Each roster row carries the one-line finding and, where one exists, the filed-issue
link; the issue holds the full mechanism, reproduction, and suggested fix.

This document is a snapshot, not a judgment or verdict: each observation describes the pinned commit in
[`SNAPSHOTS.md`](SNAPSHOTS.md) (or the commit named in the finding), and a project's current
`main` may already differ. **We draw no conclusion about engineering quality or fitness for any
use case** — every finding is offered back, not aimed at anyone.

**‡** = authored by a professional trading-industry engineer or firm — a **personal side project with no
commercial intent, not their employer's work**, except where a finding explicitly labels an official
vendor/org repo. Affiliations are as the authors state them publicly.

## How the harness probes correctness

Two signals per engine per scenario: the **report-stream hash** (SHA-256 over the full output
stream, stable-sorted, against the byte-identical consensus first established from three
independent engines) and a **192-point book-state audit** (`best_bid` / `best_ask` / `depth_at`
against a baseline replay). A run is **VALID** only when both match; any byte difference makes it
**INVALID** — meaning only "the output diverges from the consensus," **not a judgment of quality**
(`docs/ANTI_CHEAT.md`). Findings are tagged **hard-invariant violation**,
**price-time-priority violation**, or **engine-state corruption**.

## Per-engine roster

| Engine | Lang | License | Worst-case M/s | Status | Finding (one line) | Issue |
|:--|:--|:--|--:|:--|:--|:--|
| FlashOne | C++ | — | 33.20 (normal) | conforms | reference target — the harness publisher's production engine | — |
| e820 / weekend-orderbook | C | — | 8.19 | conforms | singly-linked level (`->prev` unset) orphans on cancel; fills stamped at aggressor price | [#1](https://github.com/oldfifteenpoundy/weekend-orderbook/issues/1) |
| cpp-orderbook | C++ | — | 7.94 (swing-25) | conforms | — (pinned commit already carries the price-cross fix) | resolved |
| melin | Rust | BSL-1.1 | 7.86 | conforms | the stop-order trigger cascade is single-pass (chained stop triggers are not re-evaluated) | [#2](https://github.com/melin-engine/melin/issues/2) |
| CppTrader | C++ | — | 7.26 (normal) | conforms | — on canonical; a `ModifyOrder` crash off the canonical path is fixed upstream | resolved ([#42](https://github.com/chronoxor/CppTrader/issues/42)) |
| raymondshe | Rust | MIT/Apache | 7.20 | conforms | a phantom zero-qty match corrupts the next resting order's id | [#1](https://github.com/raymondshe/matchengine-raft/issues/1) |
| Kautenja | C++ | — | 6.88 (normal) | conforms | duplicate live order-id → unchecked `emplace` re-inserts the first order (self-linked FIFO, double-counted volume, UAF on cancel) — fix verified, VALID ×5 across 100 seeds | [#4](https://github.com/Kautenja/limit-order-book/issues/4) |
| ndfex ‡ | C++ | — | 6.825 (swing-25) | conforms | — (std::map RB-tree book, clean); author: an ex-Citadel Securities engineer (17y in HFT) | — |
| mtengine | Rust | — | 6.82 (static) | non-conforming | flat array + bitmap, bounded price domain — diverges on `flash-crash` / the widest swings | — (no draft) |
| matchcore | Rust | Apache-2.0/MIT | 6.58 | conforms | a marketable limit passes `None` as its limit to `match_order`, so it sweeps every opposite level like a market order and pays through its own limit price | [#167](https://github.com/minyukim/matchcore/issues/167) |
| chronex | C++ | MIT | 6.47 | conforms | FOK / AON makers fill at the aggressor's price, not the maker's | [#1](https://github.com/OsamaAhmad00/ChroneX/issues/1) |
| yashkukrecha ‡ | C++ | none | 6.26 (normal) | conforms | — (clean; two `priority_queue`s with a timestamp FIFO tiebreak; fastest pro-wave conformer); author: incoming at Jump Trading | — |
| lobsim | C++ | — | 6.07 | conforms | — (`flat_hash_map` + Boost intrusive list + max-heaps; conforms as shipped) | — |
| asthamishra | Rust | — | 5.60 (flash-crash) | conforms | direct-indexed array, bounded 100k-tick domain — drops orders above the ceiling on wide swings — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/AsthaMishra/matching-engine/issues/1) |
| llc993 | Rust | — | 5.43 (swing-40) | conforms | — (BTreeMap + slab pool + intrusive time-queue) | — |
| newbigdeng | C++ | Apache-2.0 | 5.38 | conforms | won't compile at HEAD — a `flushQueue` brace imbalance closes `class Logger` early, plus two stale `getNextToRead`→`try_pop` call sites | [#1](https://github.com/newbigdeng/TradeSystem/issues/1) |
| johannestampere | C++ | none | 5.31 | conforms | `get_best_price()` returns 0 for an empty book (indistinguishable from a real best at 0), but every call is guarded by `!empty()` so no exercised path misbehaves | — |
| cheetah | Rust | — | 5.25 (normal) | non-conforming | cancel searches the opposite book (every cancel fails + leaks); a 10k-id de-dup window drops ~97% of non-monotonic ids | [#1](https://github.com/CheetahExchange/orderbook-rs/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| bozoslav | C++ | — | 5.01 | conforms | — (per-side price array + slab pool + id index, native IOC/FOK/modify; adapter widens the bounded band only) | — |
| hroptatyr/clob | C | — | 4.73 (normal) | conforms | — (b+tree CLOB, `_Decimal64`, no patch) | — |
| onewhitedevil | C++ | MIT | 4.73 | conforms | `cancel` never returns the order's slab slot → the pool drains → `std::bad_alloc` | [#1](https://github.com/1WHITE-DEVIL/lob-matching-engine/issues/1) |
| slmolenaar | C++ | — | 4.50 | conforms | `CancelOrder` swap-and-pop breaks FIFO time priority | [#3](https://github.com/SLMolenaar/orderbook-simulator-cpp/issues/3) |
| forever803 | C++ | none | 4.33 | conforms | — (conforming; the matcher ships inside a demo/example harness) | — |
| ranjan2829 | C++ | — | 4.07 | conforms | 4 memory-safety defects (null-deref, OOB ring, wedging pool, uninitialised index arrays) | [#3](https://github.com/ranjan2829/High-Frequency-Trading-Exchange-Engine/issues/3) |
| mercury | C++ | — | 3.94 (normal) | conforms | — (abseil b-tree) | — |
| rust_ob | Rust | MIT | 3.73 (static) | conforms | `process_market_order`'s `Decimal::MAX` sentinel overflows `rust_decimal` in the cost line → panic; unreachable through the harness (bounded ticks) | [#1](https://github.com/toyota-corolla0/rust_ob/issues/1) (latent) |
| faulaire | C++ | — | 3.62 | conforms | — (Boost.MultiIndex: hashed id + ordered price; conforms as shipped) | — |
| microexchange | C++ | — | 3.62 (flash-crash) | conforms | — (array + bitmap) | — |
| daniele ‡ | C++ | GPL-3.0 | 3.60 (static) | conforms | latent — on an exact-size fill the taker-side fill report reads the maker freed earlier in the same fill (`MatchingEngine.h:335` free → `:342` read; ASan-confirmed); the matching itself is price-time correct, the defect is on the fill-reporting path only; author: an Optiver engineer | [#1](https://github.com/Daniele122898/Trading-Engine/issues/1) |
| nanobook | Rust | MIT | 3.52 | conforms | — (clean) | — |
| Tzadiko | C++ | — | 3.39 (flash-crash) | conforms | self-deadlocks on a partially-filled IOC; two-site `CancelOrderInternal` patch → VALID ×5 (+ teardown lost-wakeup, filed separately) | [#11](https://github.com/Tzadiko/Orderbook/issues/11) + [#12](https://github.com/Tzadiko/Orderbook/issues/12) |
| timothewt | C++ | MIT | 3.34 | conforms | `prev` and `next` are both `shared_ptr` → a reference-cycle leak | [#1](https://github.com/timothewt/OrderBook/issues/1) |
| piyush | C++ | — | 3.28 (flash-crash) | conforms | report stream byte-identical on all five; asymmetric cached best-ask staleness fails the state audit on the moving scenarios — fix verified, VALID ×5 across 100 seeds | [#9](https://github.com/PIYUSH-KUMAR1809/order-matching-engine/issues/9) |
| lanpishu | C++ | — | 2.66 (static) | non-conforming | broken RB-tree delete-fixup → wrong best price/counterparty + under-match (can hang); depth double-decrement → phantom levels | [#2](https://github.com/lanpishu6300/crypto-exchange/issues/2) |
| serum ‡ | Rust | Apache-2.0 | 2.625 (static) | conforms | — (de-chained Solana CLOB; Project Serum, the original Solana on-chain order book) | — |
| fmstephe | Go | — | 2.48 (static) | conforms | crossing trades print at the bid-ask midpoint, not the maker; adapter applies the maker-price correction — fix verified, VALID ×5 across 100 seeds | [#11](https://github.com/fmstephe/matching_engine/issues/11) |
| femto_go | Go | — | 2.24 (normal) | non-conforming | VALID on `static`/`normal`; diverges deterministically on the three wide-swing scenarios (price-window limit; same hash on re-run, batched == one-at-a-time, so it is the engine's) | — (limitation, not filed) |
| parity ‡ | Java | — | 2.21 | conforms | — (RB-tree: `TreeSet` + fastutil id-map; conforms as shipped); Parity Trading's own engine (org bio: 'Open source trading technologies') | — |
| dazzz1 | Java | none | 2.16 (static) | conforms | `processOrder` clears a sell aggressor at its own (lower) limit instead of the resting bid it traded against | [#1](https://github.com/Dazzz1/warp-exchange/issues/1) |
| shivaganapathy ‡ | C++ | MIT | 2.15 (normal) | conforms | — (clean; two `priority_queue`s with a timestamp FIFO tiebreak); author: an IMC engineer | — |
| manifest ‡ | Rust | GPL-3.0 | 2.145 (static) | conforms | — (de-chained production Solana CLOB) | — |
| michaelliao | Java | — | 2.09 | conforms | — (`TreeMap` RB-tree per side; conforms as shipped) | — |
| ssuchichen | Go | — | 2.09 (normal) | conforms | lost trades + `concurrent map writes` fatal + trades priced at the sell side — all three fixed | [#1](https://github.com/ssuchichen/order-matching/issues/1) |
| maxe | C++ | — | 1.99 | conforms | — (deque-of-lists price-time + id map; a latent partial-cancel no-op, no observable impact) | — |
| coralme ‡ | Java | — | 1.97 (flash-crash) | conforms | —; CoralBlocks' own open-source engine (trading-tech vendor) | — |
| robaho | C++ | — | 1.90 (swing-25) | conforms | trade priced at the aggressor's limit, not the maker's (price-priority); `static` passes — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/robaho/cpp_orderbook/issues/2) |
| geseq | Go | — | 1.81 (swing-25) | conforms | — (a multi-level cross-through is fixed upstream) | resolved ([#25](https://github.com/geseq/orderbook/issues/25)) |
| makersu | Go | — | 1.80 (swing-25) | non-conforming | FIFO fix applied, but a separate RB-tree iterator invalidation makes multi-level sweeps skip levels | [#1](https://github.com/makersu/go-exchange-matching/issues/1) |
| gocronx | Rust | — | 1.77 (static) | conforms | — | — |
| robdev ‡ | Rust | none | 1.76 (static) | conforms | cancel leaves an emptied price level in `price_level_map` → stale `best_price()` (the match path's emptiness guard is provably immune); `remove_order` returns `true` unconditionally, so unknown-id cancels are acknowledged (`NotFound` is dead code); `TimeInForce::IOC` is parsed but never read — an IOC residual rests like GTC; author: a CME Group engineer | [#1](https://github.com/rob-DEV/match-engine/issues/1) |
| stocksharp ‡ | C# | — | 1.64 (swing-25) | conforms | at the pinned commit, same-price resting orders could match out of arrival order after an interior cancel — `Dictionary` enumeration replaced FIFO/time priority in `OrderBook`/`OrderMatcher`; conforms with the documented one-line fix; author: StockSharp / trading-tech vendor | [#681](https://github.com/StockSharp/StockSharp/issues/681) |
| jiang | Java | — | 1.63 (swing-25) | conforms | `cancelOrder` never prunes the id index → modify drops orders then crashes; 1-line `idMaps.remove(id)` → VALID ×5; worst case via bidirectional `engine_on_batch` (per-message JNI delivery 1.30) | [#3](https://github.com/JiangYongKang/FastMatchingEngine/issues/3) |
| apex | Rust | — | 1.62 (static) | conforms | crossing fills priced at the aggressor's limit, not the maker's; `static` passes — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/crypto-zero/apex-engine/issues/3) |
| kartikeya | C++ | — | 1.61 | conforms | `OrderIndex::erase` backward-shift corruption → wrong cancel / `find()` can loop forever | [#1](https://github.com/Kartikeya2710/order-matching-engine/issues/1) |
| charles | Java | — | 1.60 | conforms | won't compile (missing `UUID` import) + matcher aborts on every complete fill (`withReducedQuantity(0)`) | [#1](https://github.com/CharlesMfouapon/limit-order-book/issues/1) |
| matchina ‡ | Rust | — | 1.60 (static) | conforms | phantom zero-quantity trades (no taker-exhaustion guard in the level loop) — fix verified, VALID ×5 across 100 seeds; author: at GSR (crypto market maker) | [#3](https://github.com/fran0x/matchina/issues/3) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| harsh4786 | Rust | — | 1.60 (static) | conforms | — (a Solana agnostic order book run off-chain in-process; clean, full 100-seed × 2 M consensus as shipped) | — |
| xingxing | Java | — | 1.58 | conforms | `cancelOrder` never evicts `oidMap` → 2nd cancel NPE; won't compile (stray `javafx` import) | [#1](https://github.com/crazyzym/xingxing-match-trading/issues/1) |
| rishib064 | Rust | — | 1.55 | conforms | executions decrement quantities but emit no trade (`add_order` returns `()`); the adapter supplies the missing trade-emit + cancel, matching logic itself unchanged; also a 1-char compile fix | [#1](https://github.com/RishiB064/Rust-Limit-Order-Book/issues/1) |
| phoenix ‡ | Rust | MIT | 1.5 (static) | conforms | — (de-chained production Solana CLOB; Ellipsis Labs, founders ex-Jane Street/Citadel) | — |
| tembolo ‡ | C | — | 1.475 (swing-25) | conforms | two capacity ceilings — an 8192-order pool silently drops + a 512 price-level cap aborts; author: a quantitative developer at Tradeweb | [#1](https://github.com/tembolo1284/matching-engine-c/issues/1) |
| cryptonstudio | Go | MIT | 1.47 | conforms | clean — a quote-locking pricing observation was investigated and dropped | — |
| Exchange-core | Java/JVM | — | 1.40 (flash-crash) | conforms | consensus anchor (direct-access book); worst case with `engine_on_batch` — per-message JNI delivery 1.20 | — |
| loom | Rust | none | 1.39 (static) | conforms | FOK fillability is checked per-resting-maker, so a FOK spanning >1 maker is killed even when the book can fully fill it | [#1](https://github.com/AlphaGodzilla/loom/issues/1) |
| piquette | Go | — | 1.28 (flash-crash) | non-conforming | bid/ask maps never populated → every cancel rejected; duplicate-price stranding; best-ask uses `>` not `<` | [#2](https://github.com/piquette/orderbook/issues/2) |
| sadhbh | C++ | — | 1.28 (static) | conforms | order at the exact touch never crosses; opposite-side guard dereferences an empty book (UB) — fixed | [#6](https://github.com/sadhbh-c0d3/cpp20-orderbook/issues/6) |
| koral ‡ | C++ | MIT | 1.255 (normal) | conforms | — (FIX exchange; thread-affinity plumbing only); author: a Coinbase software-engineering intern | — |
| magenta_mice | C++ | — | 1.17 | conforms | — (`std::map` price → deque per side, native FAK/IOC; conforms as shipped) | — |
| limitbook | Rust | — | 1.16 (static) | conforms | partial fill never decrements the resting maker → ~4.3× over-match (quantity not conserved) — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/solarpx/limitbook/issues/1) |
| m5487 | Go | — | 1.15 (swing-25) | conforms | — (skiplist + disruptor) | — |
| cjboxing | Java | none | 1.12 | conforms | a fully-filled order is removed from its `PriceBucket` but never from `orderMap`, so a later cancel/modify of that filled id is acked instead of rejected | [#1](https://github.com/cjBoxing/match/issues/1) (filed; repo since inaccessible — 404 on 2026-06-29) |
| e2q | C++ | — | 1.08 (swing-25) | non-conforming | `Market::match()` silently discards any order that doesn't fully cross within one call — non-price-time by design (architectural) | [Suggestion] [#1](https://github.com/E2Quant/e2q/issues/1) |
| dx1ngy | Java | MIT | 1.07 | conforms | `match()` prices every fill at the sell order's price, so a sell crossing a higher bid prints the aggressor's lower price (measured with the maker-price fix) | [#1](https://github.com/dx1ngy/trading/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| ironcrypto ‡ | Rust | none | 1.04 (6.2 normal) | conforms | README advertises a `cancel` API that was removed from the engine; the adapter restores the engine's own removed cancel impl to measure it (faithfulness caveat); author: self-described 'TradFi/DeFi Quant' | — |
| javalob ‡ | Java | MIT | 1.03 (swing-40) | conforms | worst case with bidirectional `engine_on_batch` (per-message JNI delivery 0.86); teaching LOB, clean; author: a JPMorgan engineer | — |
| shal | Go | MIT | 1.00 (static) | conforms | `Engine.execute` derives the trade price from `order.ID > other.ID` rather than the resting maker, so it prints at the aggressor's price when ids are not arrival-ordered | [#1](https://github.com/shal/orderbook/issues/1) |
| yllvar | Rust | none | 1.00 | conforms | the matcher is clean; an off-scope settlement-Merkle odd-node duplication (a non-binding state commitment) is not filed | — |
| jcwangjc | Java | none | 0.96 | conforms | `processMath` copies the taker's `turnover` into the maker (off the matched-output path) | [#1](https://github.com/jcwangjc/exchange-matching-engine/issues/1) |
| kodoh | C++ | — | 0.95 | conforms | a sell crossing a resting bid fills at the aggressor's price, not the maker's | [#17](https://github.com/Kodoh/Orderbook/issues/17) |
| trusted ‡ | Rust | — | 0.925 (static) | conforms | latent — a bid-side market order double-subtracts the fill (u64 underflow); author: a KRX market-maker at IBK Securities | [#9](https://github.com/JunbeomL22/trusted/issues/9) |
| ffhan | Go | — | 0.89 | conforms | `Cancel` only soft-flags → cancel + re-add of the same id rejected → later match panic | [#4](https://github.com/ffhan/tome/issues/4) |
| kennethzhang ‡ | C++ | none | 0.86 (static) | conforms | output-conforming — a buy aggressor's limit-vs-limit cross prints at the taker price, not the resting maker's (`OrderBook.cpp:132-138`); the adapter normalizes every fill to the maker price, so the report stream still conforms; author: a Squarepoint quant researcher | [#1](https://github.com/kennethZhangML/TradingClientExchange/issues/1) |
| swirly ‡ | Java | none | 0.79 (swing-40) | conforms | — (clean; no defect found — native revise only changes lots, so modify maps to cancel+reinsert per the contract); author: a trading-systems developer; co-founder of Reactive Markets | — |
| i25959341 | Go | — | 0.72 (swing-25) | conforms | `OrderSide.Volume()` over-reports after a partial fill (per-side aggregate, never read by the harness) — fix verified, VALID ×5 across 100 seeds | reported upstream (duplicate) |
| jlob | Java/JNI | — | 0.71 (static) | conforms | — (L3 RB-tree, working JNI adapter) | — |
| vdt | JavaScript | MIT | 0.71 | conforms | `assert` is never imported → `ReferenceError` on the side guard | — |
| weblazy | Go | Apache-2.0 | 0.71 (static) | conforms | `sortedset.GetFirst()` returns the header sentinel and `GetLast()` nil-derefs on an empty set → spin/panic on the first order (measured with the guard fix) | [#1](https://github.com/weblazy/trade/issues/1) |
| mh2rashi | C++ | — | 0.70 (swing-40) | conforms | `deleteOrder` guard nulls a 2-order level's survivor → list corruption/crash on all five; 1-line fix → VALID ×5 | [#4](https://github.com/mh2rashi/Trading-Engine/issues/4) |
| muzykantov | Go | MIT | 0.68 | conforms | `OrderSide.Volume()` overcounts after a partial fill | — |
| wezrule | C++ | none | 0.64 (static) | conforms | `PoolAlloc` sets `propagate_on_container_move_assignment = true` but deletes its move-assignment → `Market::operator=` won't compile | [#1](https://github.com/wezrule/WezosTradingEngine/issues/1) |
| instrument_spot | Rust | MIT | 0.60 (static) | conforms | the depth maps are pruned only on an exact `sum == 0.0`, so an `f32` residual leaves a phantom level → `best_bid` returns a price with no orders | [#1](https://github.com/Andry-RALAMBOMANANTSOA/instrument_spot/issues/1) |
| danielgatis | Go | — | 0.58 (swing-25) | conforms | `decimal.Decimal` Go map key — equal prices become distinct keys, orphaning same-price orders (~61% under-match) — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/danielgatis/go-orderbook/issues/2) |
| QuantCup | C++ | — | 0.57 (flash-crash) | conforms | consensus anchor; flat price-indexed array, domain widened to 32-bit (see note below) | — |
| gotrader | Go | — | 0.56 (swing-25) | conforms | a modify of a fully-filled order is swallowed (acked), not rejected — `ModifyOrder` swallow (conformance gate) — fix verified, VALID ×5 across 100 seeds | [#23](https://github.com/robaho/go-trader/issues/23) |
| plutus | C++ | — | 0.53 | conforms | engine self-deadlocks on its first trade (non-recursive mutex re-lock) | [#1](https://github.com/bxptr/plutus/issues/1) |
| sohaibelkarmi | C++ | none | 0.47 (7.4 static) | conforms | won't build at HEAD — 4 absent CMake sources + an undeclared `match_into` (the matcher core is fine; the adapter builds it alone) | [#6](https://github.com/sohaibelkarmi/High-Frequency-Trading-Simulator/issues/6) |
| omerhalid | C++ | none | 0.46 (static) | conforms | partial-fill depth non-conservation — `PriceLevel::total_quantity_` is adjusted only on add / full-remove, never on a partial fill, so `getTotalQuantity` over-counts (report stream byte-identical; fails the 192-pt state audit); also needs a compile fix + a trade-emit hook to integrate | [#3](https://github.com/omerhalid/Real-Time-Market-Data-Feed-Handler-and-Order-Matching-Engine/issues/3) |
| fjmurcia | Rust | — | 0.43 | conforms | a filled maker is never removed from the id index → cancel/modify of a filled order returns `Ok` (id-index-remove fix) | [#2](https://github.com/fjmurcia/orderbook-rust/issues/2) — re-sweep pending |
| vllob | Julia | — | 0.43 (static) | conforms | `_walk_order_book_bysize2!` ignored its `limit_price` (latent upstream, but the benchmark adapter exercises it) — fixed | [#10](https://github.com/Renruize12306/VLLimitOrderBook.jl/issues/10) |
| mercury | Java | MIT | 0.39 | conforms | a fully-consumed maker is never evicted from the `orders` id-index → a later cancel of that filled id is acked and NPEs | [#1](https://github.com/notayessir/mercury-match-engine/issues/1) |
| circus | C# | — | 0.39 (swing-25) | conforms | reusing a completed order id crashes; a partial-fill-then-modify silently rests short; `Process()` swaps `Price`/`TriggerPrice`; fixed | [#1](https://github.com/seanoflynn/circus/issues/1) |
| masroor47 | Python | none | 0.38 (static infeasible) | non-conforming | `add_order` rebuilds the trade history with `pd.concat` every call → O(n²) | [#2](https://github.com/masroor47/limit-order-book/issues/2) |
| damian ‡ | Kotlin | Apache-2.0 | 0.38 (static) | conforms | — (clean); author: a developer with 20y at a bank | — |
| arjun | C++ | — | 0.36 | non-conforming | price priority across levels not enforced — `sortOrders()` is written but never called | [Bug Report] [#1](https://github.com/ArjunXvarma/mini-quant-trading-engine/issues/1) |
| shaunlwm | TypeScript | MIT | 0.33 | conforms | `OrderTree.updateOrder` removes the order twice, double-decrementing the level length so a still-populated level is deleted, orphaning siblings | [#1](https://github.com/ShaunLWM/LimitOrderBook/issues/1) |
| dsirotkin | C++ | — | 0.31 (static) | conforms | cancel range-erases the rest of the price level; only fires off the canonical path — fix verified, VALID ×5 across 100 seeds | drafted — repo archived, unfiled |
| pyob ‡ | Python | — | 0.30 (swing-25) | conforms | deque IndexError on a full fill + stale best_price after a cancel empties the level; author: an FX e-trading quant at mBank | [#1](https://github.com/wegar-2/pyob/issues/1) |
| omx | C | none | 0.29 | conforms | `skiplist_release` leaks the node header + struct | [#2](https://github.com/0xae/omx-engine/issues/2) |
| viabtc | C | — | 0.29 | conforms | — (skiplist + dict; conforms as shipped) | — |
| joaquinbejar ‡ | Rust | MIT | 0.29 (static) | conforms | Book::replace's lose-priority path never re-matches → rests a crossed book; author: a quant developer at Capital Delta | [#59](https://github.com/joaquinbejar/hft-clob-core/issues/59) |
| oceanbook ‡ | Go | — | 0.26 (flash-crash) | conforms | `Depth` writes quantity into the price field; off the match path (matching byte-identical) — fix verified, VALID ×5 across 100 seeds; author: self-described HFT developer (bio 'HFT / C++ / Go'; @spectra-fund) | [#44](https://github.com/draveness/oceanbook/issues/44) |
| silue | Python | none | 0.26 | conforms | `get_pnl` computes `Decimal × None` → crash on the first cross | [#1](https://github.com/silue-dev/limit-order-book-market-making/issues/1) |
| brprojects | C++ | — | 0.23 (swing-25) | conforms | — (uncached-height AVL) | — |
| dyn4mik3 | Python | — | 0.23 (swing-25) | conforms | `get_volume_at_price` calls a non-existent `get_price` → crash on depth query; 1-line fix → VALID ×5 | [#22](https://github.com/dyn4mik3/OrderBook/issues/22) |
| landakram | Rust | MIT | 0.22 (static) | conforms | clean (no reachable defect; two latent unreachable observations dropped) | — |
| lightning | C++ | MIT | 0.22 (static) | conforms | `matchAskLimit` prices fills at the aggressor's limit, not the maker's resting bid (measured with the maker-price fix) | [#1](https://github.com/754liam/Lightning/issues/1) |
| mkhoshkam | Go | — | 0.18 | conforms | heap `Less` ignores the sequence number → FIFO at equal price not preserved (adapter patch restores it) | [#10](https://github.com/mkhoshkam/orderbook/issues/10) |
| rakuzen25 ‡ | C++ | none | 0.18 (flash-crash) | conforms | within-level FIFO break — `remove_order`'s swap-with-last moves a later-arrived order into the match scan's current slot, executing the rest of a price level out of arrival order (`engine.cpp:16-24` / `:80-88`); fixed → conforms on in-range tapes. A residual uint16 price ceiling on extreme flash-crash is a representational limit (QuantCup-class; see the note below), not a correctness defect; author: an Optiver intern | unfiled — reproducible from the pinned commit |
| sculd ‡ | Python | none | 0.18 (swing-25) | conforms | cancel/status of an id the book never held raises an uncaught `KeyError` (no reject path); lazy cancellation leaves phantom price levels visible to the shipped `_get_best_price` (the matching path skips cancelled heads and is unaffected); author: a quant/developer who has worked at Two Sigma | [#1](https://github.com/sculd/orderbook_practice_python/issues/1) |
| trademacher ‡ | Java/JNI | — | 0.15 (swing-25) | conforms | —; TradeMatcher's own matching engine (trading-tech vendor) | — |
| OrderBook-rs ‡ | Rust | — | 0.13 (static) | conforms | partial fill demotes the maker to the FIFO tail → wrong counterparty; quantities correct — fix verified, VALID ×5 across 100 seeds; author: a quant developer at Capital Delta | [#88](https://github.com/joaquinbejar/OrderBook-rs/issues/88) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| abyssbook | Zig | — | 0.12 (static) | non-conforming | `swapRemove` FIFO-scramble + SIMD batch over-fill + stale best-bid/ask cache (no Zig toolchain to apply the filed fix) | [#41](https://github.com/aldrin-labs/abyssbook/issues/41) |
| lirezap | Java | ISC | ~1.2–1.6 (0.11 static) | conforms | FOK fills only against the single best level → a multi-level-fillable FOK is killed | [#1](https://github.com/lirezap/OMS/issues/1) |
| dydx ‡ | Go | GPL-3.0 | 0.11 (swing-25) | conforms | — (de-chained v4 memclob, accessor-only); author: a dYdX founder | — |
| vincurious | Java | none | 0.10 (static) | conforms | the ask comparator orders highest-price-first, so a marketable buy fails to cross the best ask and rests into a crossed book (measured with the inversion fix) | [#4](https://github.com/vinCurious/OrderMatchingEngine/issues/4) |
| volt | Zig | — | 0.10 | conforms | — (RB-tree + flat-array + hierarchical bitset, novel) | — |
| dgtony | Rust | — | 0.09 (static) | conforms | id-gen wraps [1,1000] (1001st order dropped); amend leaves the book crossed — fix verified, VALID ×5 across 100 seeds | [#9](https://github.com/dgtony/orderbook-rs/issues/9) |
| shivamkachhadiya | C++ | — | 0.09 | conforms | a crossing limit matches only at its own price level → book left crossed (best-first-sweep fix — a larger, function-local patch) | GitLab, filing pending — re-sweep pending |
| qa-rs ‡ | Rust | — | 0.09 (static) | conforms | OrderQueue lazy-deletion (stale heap entry on reinsert; `get_depth` over-counts a plain cancel) + 5 latent `Orderbook` match-loop bugs off the limit-only workload (1000-id recycle drops orders; market remainder rested not killed; amend skips the crossing check; NaN price passes validation; per-order sweep recursion → stack overflow); author: a private-fund manager (Shanghai Binghao) | [#1](https://github.com/yutiansut/qa-rs/issues/1) [#2](https://github.com/yutiansut/qa-rs/issues/2) [#3](https://github.com/yutiansut/qa-rs/issues/3) [#4](https://github.com/yutiansut/qa-rs/issues/4) [#5](https://github.com/yutiansut/qa-rs/issues/5) [#6](https://github.com/yutiansut/qa-rs/issues/6) [#7](https://github.com/yutiansut/qa-rs/issues/7) |
| jugutier | Java | none | 0.08 (static infeasible) | non-conforming | `PriorityOrderBook.update()` null-derefs when the order's side has no resting queue yet | [#1](https://github.com/jugutier/OrderBook/issues/1) |
| matchingo | Go | — | 0.08 (static) | conforms | report stream correct; `UpdateVolume` subtracts the remainder not the consumed qty → depth audit fails — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/GOnevo/matchingo/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| php_matcher | PHP | none | 0.08 | conforms | float→int array-key truncation merges distinct sub-integer prices into one price level | issues disabled — unfileable |
| vega ‡ | Go | AGPL-3.0 | 0.08 (static) | conforms | — (accessor-only); author: a Vega Protocol founder (designed a London Stock Exchange matcher) | — |
| bexchange | Go | — | 0.07 | conforms | fill loop drops partially-filled makers / emits phantom 0-qty fills / an equal-priced buy never crosses | duplicate of [#2](https://github.com/bhomnick/bexchange/issues/2) |
| lobrs | Rust | Apache-2.0 | 0.07 | conforms | after `cancel_order` empties the best level, `best` is left `None` and never recomputed, so matching stops while deeper levels still rest | [#1](https://github.com/rafalpiotrowski/lob-rs/issues/1) |
| pantelwar | Go | — | 0.07 (static) | conforms | off-path hot-path debug logging + a `MarshalJSON` sell-side bug — fix verified, VALID ×5 across 100 seeds | [#26](https://github.com/Pantelwar/matching-engine/issues/26) |
| yihuang | Python | none | 0.06 (static) | conforms | `cancel_order`'s unguarded `self.levels[price]` raises `KeyError` once a fill has emptied the level | [#1](https://github.com/yihuang/pyorderbook/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| aodr3w | Rust | none | 0.05 (static) | conforms | `execute_match` retires a filled maker via the list-unlink primitive only — never removing it from the order index or recycling the slot — so ids leak and the zero-alloc arena OOMs | [#1](https://github.com/aodr3w/zero-alloc-lob/issues/1) |
| fractal | Java | — | 0.05 (static) | conforms | crossing fills priced at the lowest offer, not the resting maker — fixed | [#1](https://github.com/FractalFinTech/OrderBook/issues/1) |
| jeog | C++ | — | 0.05 (flash-crash) | conforms | — (flat directly-indexed price vector) | — |
| jxm35 | C++ | — | 0.05 (static) | conforms | cancel-path double-unlink corrupts the level → missed crossings + bad state; trade hook never invoked — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1), [#2](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/2) |
| turbo | Java | Apache-2.0 | 0.05 (static; ~0.6–0.9 typical) | conforms | native IOC rests its residual on an empty book; a deep same-price sweep also overflows the JVM stack via unbounded `doOrder` recursion | [#1](https://github.com/sluggard6/turbo/issues/1), [#2](https://github.com/sluggard6/turbo/issues/2) |
| baoyingwang | Java | MIT | 0.04 (static; ~1.6 typical) | conforms | a FIFO-tiebreak subtraction overflows past a 24.85-day order age; the deep-tape crash is a separate, new robustness finding | — |
| jpalounek | Python | — | 0.04 | conforms | `_balance()` mutates the AVL tree while iterating it → skips a resting order (the sweep's sole diverger; AVL snapshot-iteration fix) | [#1](https://github.com/JPalounek/order-book/issues/1) — re-sweep pending |
| betting_exchange | Scala | none | ~0.03 (seed-23) | conforms | a cancelled bet's id is retained forever (`betsIds` never pruned) | — |
| cointossx | Java/JNI | — | 0.03 (static) | conforms | B+Tree destructive `firstKey` (best collapses to 0 past 100 levels) + `AddOrderPreProcessor` wrong-side compare; 2 fixes → VALID ×5 | [#10](https://github.com/dharmeshsing/CoinTossX/issues/10) |
| dhyey | C++ | MIT | 0.03 (static) | conforms | `delete_order` uses `order_map[id]` (`operator[]`), inserting then dereferencing a null `Order*` → SIGSEGV on a cancel of a non-resting order; separately, `Limit::remove_order` decrements the level volume but never unlinks the emptied level, so a cancel at the best price leaves a stale zero-volume top until a later match sweeps it (book-state audit; self-heals before any trade) | [#1](https://github.com/Dhyey-Mehta/order-book/issues/1) [#2](https://github.com/Dhyey-Mehta/order-book/issues/2) |
| Liquibook | C++ | — | 0.03 (static) | conforms | consensus anchor; native IOC residual rests instead of cancelling | [#43](https://github.com/enewhuis/liquibook/issues/43) |
| philipgreat | Rust | — | 0.03 (static) | conforms | three cancel/modify-path issues (zero-qty phantoms, tombstone cancel, id-index disown); three patches → VALID ×5 | [#1](https://github.com/philipgreat/lighting-match-engine-core/issues/1), [#2](https://github.com/philipgreat/lighting-match-engine-core/issues/2), [#3](https://github.com/philipgreat/lighting-match-engine-core/issues/3) |
| amer ‡ | Java | none | 0.02 (static) | non-conforming | same-price orders execute LIFO, not FIFO — `>=`/`<=` insertion splices a new order ahead of its equal-priced peers, failing exactly the gate's four tie-break cases and diverging on 33 of 40 sweep cells; the matcher also never exits early (unconditional O(depth) contra scan), so deep 2 M runs take ~9–10 min, some exceeding the watchdog; author: a former Nasdaq engineer | [#1](https://github.com/AmerSurkovic/MatchingEngine/issues/1) |
| mkxzy | Java | none | 0.02 (static) | conforms | clean (no reachable defect; no license) | — |
| rabbittrix | Rust | — | 0.02 | conforms | bid `binary_search_by_key` mismatches its sort key → bids unsorted, marketable sells don't cross; `cancel_order` is a stub | [#1](https://github.com/rabbittrix/Ultra-Low-Latency-FX-eTrading-Platform/issues/1) |
| rhodey | JavaScript | MIT | ~0.02–0.12 (swings) | conforms | — (clean) | — |
| auralshin | Rust | — | 0.01 (static) | conforms | — | — |
| zorrofix ‡ | C++ | BSD-3 | 0.01 (static) | conforms | sweep loop never checks whether the aggressor has closed → execute(0)→NAN→abort; DSEC Capital's own repo (org bio: 'Algo Trading Tech Shop') | [#12](https://github.com/dsec-capital/zorro-fix/issues/12) |
| gavincyi | Python | — | 0.01 (static) | non-conforming | crashes on a cancel/modify of a fully-filled order — stale `order_id_map` entry → AssertionError (conformance gate) | [#18](https://github.com/gavincyi/LightMatchingEngine/issues/18) |
| pyme | Python | — | infeasible (2 M) | non-conforming | infeasible at 2 M — stalls past the 600 s watchdog on a stressful flash-crash tape (its 0.31 M/s swing-25 was seed-23); separately, a deep same-price sweep crashes `Orderbook.send()` (trade-log grows by a fixed increment, not the shortfall — engine's own API, `ValueError`) | [#48](https://github.com/Surbeivol/PythonMatchingEngine/issues/48) |
| lll | C++ | none | infeasible (static) | non-conforming | infeasible at 2 M — stalls at the 600 s watchdog on deep static 2 M (seed 11930289) at both the uint16 and a widened build (quiet-box A/B); `modify_order_in_map` no-op is a separate latent issue | [Suggestion] [#1](https://github.com/northwesternfintech/low-latency-league/issues/1) |
| prystupa | Scala | none | infeasible (static) | non-conforming | infeasible at 2 M — a deep stressful-static book stalls past the 600 s watchdog (its 0.02 M/s was seed-23 static); the latent `FastList.removeInto` tail-drop is filed separately | [#6](https://github.com/prystupa/scala-cucumber-matching-engine/issues/6) |
| cspooner | Rust | — | very slow (static) | conforms | partial fill deletes both orders wholesale (quantity not conserved); + wrong-side ask cancel, ask pricing, no time priority, 2dp rounding — fix verified, VALID ×5 across 100 seeds | [#14](https://github.com/christian-spooner/trading-server/issues/14) |
| dabrowdev | TypeScript | MIT | very slow (static) | conforms | O(n²) cancel/sweep — `OrderQueue.remove` reindexes the whole level (its own performance bug) | Codeberg — filing TODO |
| darkpool | C++ | — | very slow (static) | conforms | aggressor pricing in the execution-price report — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/dendisuhubdy/dark_pool/issues/1) |
| fasenderos | TypeScript | — | very slow (static) | conforms | — (libnode embed; very slow in its weakest scenario) | — |
| ghosh ‡ | C++ | MIT | very slow | conforms | flat 256-slot price index shared by both sides has no collision handling → two levels 256 ticks apart (or a bid/ask aliasing one slot) share a bucket, teardown gets the side wrong → dangling pointer → later crash; + an order-id table capped at 1,048,576 with no bounds check (throws out of a `noexcept` fn); both fixed; author: a low-latency trading-systems developer and Packt author | [#9](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP/issues/9) |
| khrapovs ‡ | Python | MIT | very slow (pure Python) | conforms | `orders_by_expiration` not pruned on fill; author: a senior ML engineer at ING (bank) | [#25](https://github.com/khrapovs/OrderBookMatchingEngine/issues/25) |
| lightning | Go | — | very slow (static) | conforms | skiplist `Delete` predecessor corruption loses resting orders → cancels/modifies wrongly rejected (nondeterministic) — fix verified, VALID ×5 across 100 seeds | reported upstream (duplicate) |
| lobster | Rust | — | very slow (swing-40) | conforms | — (BTreeMap + arena + id-index; very slow in its weakest scenario) | — |
| lsamber | Java | none | very slow | conforms | same-price FIFO breaks on a `LocalDateTime.now()` collision inside a non-stable `PriorityQueue` | [#1](https://github.com/LS-Amber/financial-trading-system/issues/1) |
| luo4neck | C++ | — | very slow (static) | conforms | fills the first time-ordered crossing, not the best price (price-time violation); `static` passes — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/luo4neck/MatchingEngine/issues/3) |
| m15102785298 | Java | none | very slow | conforms | `selSearch`/`buySearch` binary-search a `LinkedList` via `get(mid)` (O(n)) → O(n² log n) deep-book build (the insertion result is correct, only its cost is pathological) | [#1](https://github.com/15102785298/Matching-algorithm/issues/1) |
| mansoor | C++ | — | very slow (normal) | conforms | OOB on a wide swing — unchecked bounded price array crashes on `flash-crash` (clean in-band; VALID on the two scenarios it can finish within budget) — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/mansoor-mamnoon/limit-order-book/issues/3) |
| ms_engine | TypeScript | none | very slow | conforms | a no-liquidity market order rests its remainder at sentinel price −1 → phantom level (market-order path only; the limit-only workload never reaches it) | — |
| pgellert | Rust | — | very slow (swing-40) | conforms | `check_for_trades` drops a popped order; stale price-bounds hide marketable orders → ~70% under-match — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/pgellert/matching-engine/issues/2) |
| pyxchange | C++ | — | very slow (static) | conforms | wall-clock `(price,time)` key drops same-tick same-price orders — fix verified, VALID ×5 across 100 seeds | drafted — issues disabled, unfiled |
| ridulfo | Python | — | very slow (static) | conforms | `LimitOrder.__lt__` is not a consistent total order → priority inversion + lost cancels on time ties — fix verified, VALID ×5 across 100 seeds | [#10](https://github.com/ridulfo/order-matching-engine/issues/10) |
| techieboy | Rust | — | very slow (swing-25) | conforms | spurious zero-qty fills + stale best-bid/ask; 2 fixes → VALID ×5 | [#1](https://github.com/TechieBoy/rust-orderbook/issues/1) |
| aas2015001 | Java | no license | — | non-conforming | an exact fill leaves a zombie 0-qty order, and each price level holds only one order | [#1](https://github.com/aas2015001/OrderMatchingEngine/issues/1) |
| afterworkguinness | Java | none | infeasible (2 M) | non-conforming | builds a whole-book String on every insert and match even with tracing off → O(book)/op (conforms on every cell it completes) | [#2](https://github.com/afterworkguinness/matching-engine/issues/2) |
| amansardana | Go | MIT | — | non-conforming | the fill loop lacks an `o.Amount > 0` guard, so it over-sweeps: a spurious zero-amount fill, and the order resting behind it is unlinked from the book | [#1](https://github.com/amansardana/matching-engine/issues/1) |
| apexmatch | Go | none | infeasible (2 M) | non-conforming | won't build at HEAD — six `.go` files are committed without their `package` clause and one function body is split across files | [#1](https://github.com/luka2049/apexmatch/issues/1) |
| bahbah94 | Haskell | — | — | non-conforming | computes trades but never removes the filled liquidity from the book | [#1](https://github.com/bahbah94/Order-Book-Haskell/issues/1) |
| big_order_book | JavaScript | GPL-3.0 | — | non-conforming | a fully-filled maker is detached but never removed from `orderItemMap`, so a later cancel/modify of that id dereferences a null list (a harness crash); GPL handling = glue shipped, engine fetched at build | [#1](https://github.com/Capitalisk/big-order-book/issues/1) |
| buttercoin ‡ | CoffeeScript | MIT | crash | non-conforming | a partial residual is re-inserted under an inverted price key → the book locks; the engine Buttercoin (a Bitcoin exchange that closed in 2015) open-sourced | [#9](https://github.com/buttercoin/buttercoin-engine/issues/9) |
| chessbr | Rust | — | — | non-conforming | sells matched the lowest resting bid, not the best — fixed, but the engine is O(n²) and cannot finish 2 M | [#4](https://github.com/chessbr/rust-exchange/issues/4) |
| cltwski | C++ | — | — | non-conforming | a partially-filled resting order's quantity is never reduced (matching runs against a disconnected copy of the order list) → over-fill / non-conservation | [#1](https://github.com/cltwski/OrderBookSimulatorWithOpenCL/issues/1) |
| coinexchange | Java | — | infeasible | non-conforming | byte-identical on every completed cell; too slow to finish its worst scenario in budget (claimed stale-modify-ack did not reproduce) | — |
| devashishpuri | TypeScript | no license | — | non-conforming | a sell sweeping ≥3 bid levels refreshes its inner loop with `getObjMin` instead of `getObjMax`, so after the top bid it jumps to the cheapest remaining bid (out of price order) | [#1](https://github.com/devashishpuri/ExchangeMatchingEngine/issues/1) |
| dylanlott | Go | MIT | — | non-conforming | `MatchOrders` dereferences `buyOrders[0]` before checking the buy side is non-empty → a sell into an empty book panics (the underlying non-conformance — a descending sell-side sort and a wrong-side fill quantity — is left intact) | [#10](https://github.com/dylanlott/orderbook/issues/10) |
| glinscott | JavaScript | no license | — | non-conforming | cancelling the last order strands an empty `Limit`; the next access null-derefs (rescuable) | [#2](https://github.com/glinscott/JSOrderbook/issues/2) |
| harshsuiiii | TypeScript | ISC | — | non-conforming | `fillOrders` scans each side from the tail and `pop()`s the consumed maker, but the sides are sorted best-price-first, so it deletes an untouched bystander and matches a worse price before a better one | [#1](https://github.com/harshsuiiii/LOW-LATENCY-TRADING-MATCHING-ENGINE-ORDERBOOK-/issues/1) |
| hillside6 | Java | no license | — | non-conforming | the best bid is taken as `get(size-1)`, the newest order among same-price ties, so a sell fills most-recent-first (bid-side LIFO, not FIFO) | [#1](https://github.com/hillside6/matching/issues/1) |
| hinokamikagura | Java | no license | — | non-conforming | `updateOrderQuantity` loses the order's FIFO position | [#1](https://github.com/hinokamikagura/crypto-wallet-engine/issues/1) |
| hnodomar | C++ | no license | — | non-conforming | a `Level&` bound once is freed mid-sweep by `book.erase`, then read and written through the dangling reference → heap use-after-free, corrupting later matches at that price | [#1](https://github.com/Hnodomar/Spot-Exchange/issues/1) |
| hyobyun | JavaScript | MIT | — | non-conforming | the heap comparator reads `.price` off the `Node` (not `.key`) → `NaN` ordering | [#3](https://github.com/hyobyun/exchangeengine/issues/3) |
| ismailfer ‡ | Java | — | infeasible | non-conforming | `processTrade()` clears the wrong side's `active` flag on a full fill — truncates multi-order sweeps and orphans resting quantity (a fully-filled id can also never be reused); diverges + infeasible at 2 M; author: self-described systematic trading developer / quant trader | [#1](https://github.com/ismailfer/exchange-simulator/issues/1) |
| iwtxokhtd83 | Go | MIT | infeasible (2 M) | non-conforming | the match loop removes the consumed head only after the loop, so a zero-remaining head is returned forever → non-terminating on the first multi-fill (measured with the one-line termination fix) | [#12](https://github.com/iwtxokhtd83/MatchEngine/issues/12) |
| jenyayel | C# | MIT | — | non-conforming | `AddOrder` inserts a new same-price order at an arbitrary `List.BinarySearch` index, losing equal-price time priority | — |
| jiker_burce | Rust | no license | — | non-conforming | `Order::match_price` writes its `(Buy,Sell)` arm as the aggressor's price instead of the maker's, so a buy sweep prints every fill at the buy's price | [#1](https://github.com/jiker-burce/matching-engine/issues/1) |
| jlome | Java | MIT | — | non-conforming | `Collections.min` picks the lowest bid for a sell rather than the highest → inverted sell-side price priority (a marketable sell can rest, leaving a crossed book) | [#1](https://github.com/Alessandro-Salerno/JLOME/issues/1) |
| jogeshwar | Rust | — | infeasible | non-conforming | byte-identical on completed cells with the documented fill-size fix; too slow on its worst scenario | [#1](https://github.com/jogeshwar01/exchange/issues/1) |
| jxxxq | OCaml | — | — | non-conforming | `combine_orders` collapses same-price orders into one synthetic order → FIFO / per-order identity lost; needs a consume-path rework, not a one-line patch | [#1](https://github.com/Jxxxq/ocaml-orderbook-engine/issues/1) |
| knocte_fx | F# | MIT | — | non-conforming | a marketable limit that crosses one level then meets a non-crossing level rests over the book → crossed book + lost fill | — |
| laffini | Java | — | crash (all 5) | non-conforming | crashes all five — `\|\|` should be `&&`, plus missing price-cross / empty-list guards | [#27](https://github.com/Laffini/Java-Matching-Engine-Core/issues/27) |
| laymats | Java | no license | — | non-conforming | `getlowestTradeOrder()` returns the earliest-inserted crossable ask, not the lowest-priced one, so a buy matches the oldest/dearest ask and prints above the best offer | [#19](https://github.com/laymats/auto.trade.engie/issues/19) |
| liqian ‡ | C++ | GPL-2.0 | — | non-conforming | correct on every completed cell but **infeasible at 2 M**: `processOrder` walks the entire 20,000,000-tick price domain one bit at a time for each order (`MatchingEngine.hpp:61/76`), so the occupancy bitsets never skip empty levels — a single resting sell into an empty book already costs ~20 M probes (~14 ms); author: ex-Virtu Financial quant trader | [#1](https://github.com/QuantTradingWithLi/high_perf_order_matching/issues/1) |
| lmxdawn | Java | — | infeasible | non-conforming | byte-identical on completed cells with the phantom-fill fix; too slow on its worst scenario | [#11](https://github.com/lmxdawn/exchange/issues/11) |
| lua_matcher | Lua | no license | — | non-conforming | same-price FIFO insert violation: a later same-price order is queued ahead of earlier ones | [#1](https://github.com/geek-sajjad/crypto-matching-engine-lua/issues/1) |
| luminengine | Rust | — | — | non-conforming | async matcher thread → non-deterministic output that can't be quiesced to a deterministic order (architectural; AGPL-3.0, no bug filed) | — |
| lyqingye | Java | no license | — | non-conforming | its `normal`-tape divergence is an intrinsic notional-budget order design (a buy carries a currency budget and derives quantity from the maker price), reproduced faithfully — not a defect; a separate O(n²) non-marketable-scan performance finding is drafted but unfileable (the repository is archived) | — |
| mattdavey | Java | — | infeasible | non-conforming | `OrderBook.placeOrderInBook` rescans the resting side on every insert — O(n)/order, O(n²) total as the book deepens; infeasible at 2 M | [#26](https://github.com/mattdavey/EuronextClone/issues/26) |
| mmrath | Rust | no license | — | non-conforming | `cancel` frees the slab slot but leaves the price-level entry, so the next insert reuses the slot and the phantom level aliases a live order → a spurious cross at the wrong price | [#1](https://github.com/mmrath/oms/issues/1) |
| murtyjones | TypeScript | ISC | — | non-conforming | the divergence is the engine's documented, unit-tested "execute at the best price for the buyer" pricing convention (a fill prints at the lower of the two crossing prices); the book state is consensus-correct, so this is a convention, not a defect. No bug | — |
| nexbook | Scala | Apache-2.0 | — | non-conforming | the divergence is a deliberate midpoint deal-price convention (`(o.limit + counter.limit) / 2`); a one-line maker-price variant reproduces the exact expected hash over 1 M ops, so matching, FIFO, and quantities are consensus-correct and only the price field differs. No bug | — |
| nilesh05apr ‡ | C++ | MIT | — | non-conforming | a crossing order matches only one counterparty and strands its residual: `MatchOrder::matchOrders` advances *both* iterators after every trade (`MatchOrder.cpp:105-115`), so an aggressor that should sweep several makers fills only the first. Correctness-rescuable (a two-line change), but the matcher also re-sorts the whole resting side per order (O(n²) by the engine's own design), so it stays **infeasible at 2 M**; author: a Tower Research Capital SWE intern | [#1](https://github.com/nilesh05apr/TradeSim/issues/1) |
| nirvanasu | Go | MIT | — | non-conforming | an unstable `sort.Slice` with a price-only comparator loses same-price time priority once a side holds more than 12 orders, so the 32-case gate passes but the canonical workload diverges | [#1](https://github.com/nirvanasu00-cpu/Go-Exchange-Core/issues/1) |
| oldfritter | Go | — | — | non-conforming | limit-vs-limit never matches (`LimitTop` reads the wrong tree); once fixed, the match loop recurses forever — needs the whole fill / write-back layer reworked; separately, `removeLimitOrder` silently drops a cancel that shares its price level (never re-`Put`s the mutated level) and `LimitOrdersMap` panics on any non-empty book | [#4](https://github.com/oldfritter/matching/issues/4) [#5](https://github.com/oldfritter/matching/issues/5) |
| opencx | Go | MIT | — | non-conforming | a multi-level fill drops the residual, and a `uint64` size underflows | — |
| opexdev ‡ | Kotlin | — | infeasible | non-conforming | modify after a partial fill rests less than requested (stale `filledQuantity` carried into the reprice) + an unconditional O(book-depth) tax per message → infeasible at 2 M; OPEX's own repo (open-source crypto-exchange platform) | [#688](https://github.com/opexdev/core/issues/688) |
| osmosis ‡ | CosmWasm/Rust | — | infeasible (2 M) | non-conforming | infeasible at 2 M under the wasmer VM (byte-identical on the gated cells with the fix); latent — `Orderbook.next_bid_tick`/`next_ask_tick` are never retracted on a cancel or exact-drain sweep, so the contract's own `spot_price` reads a stale best (the gate's state dimension catches it without the fix) | [#211](https://github.com/osmosis-labs/orderbook/issues/211) |
| peatio | Ruby | — | infeasible | non-conforming | byte-identical on every completed cell; the Ruby rbtree engine exceeds the watchdog | — |
| pyobsim | Python | — | — | non-conforming | `Side.remove` deletes only the head order and `Book.__match` mutates the level while iterating it | [#2](https://github.com/jmcph4/PyOBSim/issues/2) |
| pyrsquant | Rust | MIT | — | non-conforming | `PriceLevel::remove_order` uses `Vec::swap_remove`, moving the newest same-price order into the cancelled slot, so any non-tail cancel breaks FIFO (latent on the canonical seed, surfaced by the gate) | [#1](https://github.com/tombelieber/py-rs-quant/issues/1) |
| raunakchopra ‡ | C++ | no license | — | non-conforming | **crashes**: a marketable order re-submits its remainder through the matcher recursively, so a sweep of ~12 k resting orders overflows the stack (SIGSEGV), and every op rewrites the whole `TRADES.txt` (O(trades²) blocking file I/O). It is also not price-time (it matches the first crossing entry in *insertion* order) and reports the wrong size on an ASK-initiated partial fill (`main.cpp:77`); author: a Flow Traders engineer | [#1](https://github.com/raunakchopra/OrderBook/issues/1) |
| realyarilabs | Elixir | — | — | non-conforming | an expired maker still trades, and the cancel guard is inverted | [#134](https://github.com/realyarilabs/exchange/issues/134) |
| redisexchange | C++ | — | infeasible | non-conforming | correct on every completed cell; the matching loop never stops walking the resting side once nothing more can cross — O(book depth)/order, and `active_order_invariant()` pays the identical cost again per call → infeasible at 2 M | [#15](https://github.com/jayjaychicago/RedisExchange/issues/15) |
| rinok | Clojure | EPL-1.0 | — | non-conforming | a buy crossing a lower-priced resting sell prints at the buy's price, not the maker's (buy-initiated crossings only) | [#2](https://github.com/film42/rinok/issues/2) (resolved upstream — see `RESOLVED_FINDINGS.md`) |
| shilun | Java | — | infeasible | non-conforming | overfill on partial fills (a stale live order re-matches in the same pass) + reversed buy-side priority + wrong trade price when a SELL aggresses; diverges + infeasible at 2 M | [#1](https://github.com/shilun/matchmaking/issues/1) |
| soham ‡ | C++ | none | — | non-conforming | binary Yes/No prediction-market matcher — prices are hard-validated to [1, 99] and the two ladders cross when `p_yes + p_no >= 100`, so the benchmark's ~33,500-tick tapes are rejected in full by design; internally sound (author-run differential fuzzing upstream; in-domain adapter checks match exactly), no bug report was filed; author: an iRage quant-analyst intern | — |
| thelilypad | Python | — | infeasible | non-conforming | same-price orders can fill out of arrival order (`Order.__lt__` has no tiebreak, so `heapq` doesn't preserve FIFO); diverges + infeasible at 2 M | [#10](https://github.com/thelilypad/orderbook_simulator/issues/10) |
| vinci217 | Go | none | infeasible (2 M) | non-conforming | `GetMarketDepth` returns arbitrary, unsorted price levels (it ranges a Go map with no price sort) | [#2](https://github.com/Vinci-217/trading-system/issues/2) |
| wailo | C++ | — | — | non-conforming | reflexive `operator>` (invalid `std::*_heap` comparator, UB) fixed, but no time priority + silent intake-queue drops remain | [#1](https://github.com/wailo/orderbook-matching-engine/issues/1) |
| zackienzle | C++ | — | crash (wide swings) | non-conforming | hierarchical 4-tier bitmap, bounded price domain — crashes (OOB) on `swing-40` / `flash-crash` | — (no draft) |
| zhaocong6 | Go | — | — | non-conforming | `cancel` drops every order at the price level, not just the target | [#1](https://github.com/zhaocong6/match/issues/1) |
| zzsun777 | C++ | — | infeasible | non-conforming | byte-identical on every completed cell; O(n) by-owner find exceeds the watchdog on its worst scenario | — |
| bitex ‡ | Python | — | infeasible (static) | non-conforming | correct on normal/swings, but the static deep-book scenario times out at 2 M; author: a founder of BlinkTrade (the open-source platform behind the Foxbit exchange) | — |
| eneiand | C# | none | infeasible (2 M) | non-conforming | byte-identical at reduced counts (2k–100k) and clean on the conformance gate, but the canonical 1 M is infeasible at 2 M; single-thread .NET matcher | — |
| figgie ‡ | OCaml | — | infeasible (static) | non-conforming | byte-identical where it completes, but static times out at 2 M; author: an ex-Jane Street engineer — figgie is Jane Street's *Figgie* card game (a FIFO matcher), not a commercial engine | — |
| isaaccheng ‡ | Python | — | infeasible (static) | non-conforming | byte-identical where it completes, but static times out at 2 M; author: ex-T. Rowe Price fixed-income quant developer | — |
| abides ‡ | Python | — | infeasible (static) | non-conforming | byte-identical on small cells, but static times out at 2 M; JPMorgan's research org repo | — |
| lykke ‡ | Kotlin | — | infeasible (static) | non-conforming | fast on normal; its static deep-book run exceeded the 2 M-message budget at the pinned commit (conforms on the scenarios it completes); the Lykke exchange's own engine | — |
| pylob ‡ | Python | — | infeasible (static) | non-conforming | SQLite-backed; static times out at 2 M; author: a JPMorgan engineer | [#8](https://github.com/DrAshBooth/PyLOB/issues/8) |
| alphatrade | Python | — | infeasible | non-conforming | JAX matcher, ~7 ms/message → infeasible at 2 M; author: KangOxford (academic) | [#49](https://github.com/KangOxford/AlphaTrade/issues/49) |
| konqr | Python | — | — | non-conforming | Hawkes-process simulator — not a price-time CLOB in the form the benchmark needs | [#19](https://github.com/konqr/lobSimulations/issues/19) |
| lethalazo ‡ | C++ | — | — | non-conforming | add-only book, missing the cancel/modify operations the benchmark drives; author: at Marshall Wace (hedge fund) | [#1](https://github.com/lethalazo/cpp-order-matching-engine/issues/1) |
| clober ‡ | Solidity/EVM | Apache-2.0 | 0.02 (static diverges) | non-conforming | de-chained via revm; the static deep-book scenario diverges at Clober v2's own 32,768-order-per-tick (2^15) OrderId cap — conforms 400/400 on the four feasible scenarios; a representational limit (QuantCup-class), not a correctness defect | — |
| deepbook ‡ | Sui Move | Apache-2.0 | infeasible (2 M) | non-conforming | de-chained via the Sui Move VM; a deep recursive sweep is truncated by DeepBook's own `MAX_FILLS`=100 per-call cap — a documented gas-safety design cap, not a defect (gate 32/33) + infeasible at 2 M | — |
| econia ‡ | Aptos Move | — | infeasible (2 M) | non-conforming | de-chained via the Aptos Move VM; a deep same-price sweep exceeds the 600 s watchdog (~444 ms/msg, gate 32/33) → infeasible | — |

### Matching-algorithm family coverage

| Family | OSS coverage in survey | Example engines |
|:--|:--|:--|
| Price-time / FIFO | ✅ dense (~95% of engines) | liquibook, cpptrader, robaho, cpp_orderbook, exchange_core |
| Pro-rata | ✅ | kodoh/orderbook, maxe-team/maxe, isaaruwu, SAY-5/orderbook-fix, nativa-c |
| Pro-rata + top-priority | ✅ (sub-variant) | the FIFO-&-pro-rata C++ engines that reserve a top-of-book slice |
| Call auction (opening/closing) | ✅ | xingzi2015/realstock2, matahho/tinyme, A-share engines |
| Midpoint / pegged / dark | ✅ | milczarekit/nexbook (midpoint), dendisuhubdy/dark_pool, minyukim/matchcore (pegged) |
| Iceberg / hidden / display | ✅ | minyukim/matchcore, matahho/tinyme |
| Frequent batch auction (FBA) | ❌ literature-only | — (Budish–Cramton–Shim 2015; no OSS implementation) |
| Size-priority | ❌ literature-only | — (exchange convention; no OSS) |
| Time-weighted pro-rata | ❌ literature-only | — (CME-style spec; no OSS) |


### A note on a reference engine's representational limit

The published reference hash was first established from three independent engines
(Liquibook, QuantCup, Exchange-core). One carries a correctness-relevant
robustness patch worth recording: QuantCup's flat price-indexed array uses a
fixed-width price domain, and the upstream 16-bit form (a 65,535-slot array)
**aborts at the engine's representational ceiling** when a wide-swing workload
visits a price beyond it. The baseline carries a patch widening the price word to 32 bits and
the flat array to 262,144 slots (2^18) so a wide-swing seed indexes in range instead of
aborting; it is byte-identical on the canonical seed — see `docs/PATCHES.md`.

## Reproducing

Each observation in this document is reproducible from running code in this
repository:

```bash
bash additional_references/<name>_adapter/build.sh   # clones + builds
./harness --engine <name>_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

Where `<name>` is any of the 40 adapters shipped in [`additional_references/`](additional_references/)
(~134 engines were adapted across the survey; the 40 permissively-licensed adapters ship in the
repo, the rest are built and measured but held data-only).
The build scripts pin each upstream to
the commits listed in [`SNAPSHOTS.md`](SNAPSHOTS.md). The adapters themselves are not maintained
— if any upstream advances past the pinned commit the source-level
observations here may no longer apply; treat this document as a record of
one point in time.
