# Consensus-conforming engines

Companion to [`README.md`](README.md) (which shows the top 10 by worst-case throughput). A one-line finding and the filed-issue link per engine are in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) (the linked issue carries the full mechanism and patch); the pre-run conformance gate is in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

These **140** high-confidence engines (for **85** of them, with our suggested fix) reach byte-for-byte identical consensus on both the report stream and the book state across 100 random workload seeds (**+1 billion order messages** on each engine), and each also passes the pre-run conformance gate. **as shipped** = conforms unmodified; **with fix** = conforms after the minimal documented engine patch named (the filed upstream issue; one-line finding in `CORRECTNESS_FINDINGS.md`, full mechanics in the issue). They span every common book architecture and 20+ source languages.

The **Worst-case M/s** column is each engine's lowest throughput across the five scenarios (weakest regime, seed 23, Graviton4 / Neoverse-V2, `-O3 -march=native`), measured **in isolation on clean cores** at the median of 10 trials. A parenthesised scenario names the weakest regime where recorded; the broader survey rows give the worst-of-five as a single figure.

| Engine | Language | Conformance | Worst-case M/s | Published figure | Notes |
|:--|:--|:--|--:|:--|:--|
| FlashOne | C++ | as shipped | 33.20 (normal) | — | reference target |
| e820 / weekend-orderbook | C | with fix | 8.19 | — | singly-linked orphan + aggressor-price fix [#1](https://github.com/oldfifteenpoundy/weekend-orderbook/issues/1) |
| geseq/cpp-orderbook | C++ | as shipped | 7.94 (swing-25) | — | author-contributed C++ port of geseq/orderbook |
| melin | Rust | with fix | 7.86 | — | held/BSL-1.1; stop-trigger cascade single-pass [#2](https://github.com/melin-engine/melin/issues/2) |
| CppTrader (1041★) | C++ | as shipped | 7.26 (normal) | ~7.2M upd/s | a `ModifyOrder` defect off the canonical path is fixed upstream — `RESOLVED_FINDINGS.md` |
| raymondshe | Rust | with fix | 7.20 | — | MIT-Apache; phantom zero-qty match corrupts next order's id [#1](https://github.com/raymondshe/matchengine-raft/issues/1) |
| Kautenja (309★) | C++ | with fix | 6.88 (normal) | — | reject a duplicate live order-id (no self-linked FIFO / UAF) |
| matchcore | Rust | with fix | 6.58 | — | marketable limit passes None → sweeps like market order, pays through own limit [#167](https://github.com/minyukim/matchcore/issues/167) |
| chronex | C++ | with fix | 6.47 | — | MIT; FOK/AON makers fill at aggressor price [#1](https://github.com/OsamaAhmad00/ChroneX/issues/1) |
| yashkukrecha | C++ | as shipped | 6.26 (normal) | — | two priority_queues + timestamp FIFO tiebreak (clean; fastest pro-wave conformer) |
| lobsim | C++ | as shipped | 6.07 | — | flat_hash_map + Boost intrusive list + max-heaps |
| asthamishra | Rust | with fix | 5.60 (flash-crash) | — | bounds-check the tick array — no dropped orders above the ceiling |
| llc993 (154★) | Rust | as shipped | 5.43 (swing-40) | ~7.2M/s | BTreeMap + slab pool + intrusive time-queue (exchange-core-inspired) |
| newbigdeng | C++ | with fix | 5.38 | — | won't compile — flushQueue brace imbalance + two stale call sites [#1](https://github.com/newbigdeng/TradeSystem/issues/1) |
| johannestampere | C++ | as shipped | 5.31 | — | get_best_price() returns 0 for empty book; all calls guarded, held (downgraded) |
| bozoslav | C++ | as shipped | 5.01 | — | per-side price array + slab pool + id index, native IOC/FOK/modify |
| hroptatyr/clob | C | as shipped | 4.73 (normal) | ~6M/s | b+tree CLOB, `_Decimal64` (no patch) |
| onewhitedevil | C++ | with fix | 4.73 | — | MIT; cancel never frees slab slot → bad_alloc [#1](https://github.com/1WHITE-DEVIL/lob-matching-engine/issues/1) |
| slmolenaar | C++ | with fix | 4.50 | — | CancelOrder swap-and-pop FIFO fix [#3](https://github.com/SLMolenaar/orderbook-simulator-cpp/issues/3) |
| forever803 | C++ | as shipped | 4.33 | — | held/no-license; conforming; demo-scaffolding only |
| ranjan2829 | C++ | with fix | 4.07 | — | 4 memory-safety defects fix [#3](https://github.com/ranjan2829/High-Frequency-Trading-Exchange-Engine/issues/3) |
| mercury | C++ | as shipped | 3.94 (normal) | 3.2M/s | abseil b-tree |
| rust_ob | Rust | with fix | 3.73 (static) | — | Decimal::MAX sentinel overflows rust_decimal → panic; unreachable through harness, held |
| microexchange (62★) | C++ | as shipped | 3.62 (flash-crash) | 2.24M/s | array + bitmap |
| faulaire | C++ | as shipped | 3.62 | — | Boost.MultiIndex: hashed id + ordered price |
| daniele | C++ | with fix | 3.60 (static) | — | fill-report reads a maker freed in the same fill (matching is correct); [#1](https://github.com/Daniele122898/Trading-Engine/issues/1) |
| nanobook | Rust | as shipped | 3.52 | — | MIT; clean |
| Tzadiko (307★) | C++ | with fix | 3.39 (flash-crash) | — | IOC self-deadlock; two-site lock-wrapper fix |
| timothewt | C++ | with fix | 3.34 | — | MIT; prev/next both shared_ptr → reference-cycle leak [#1](https://github.com/timothewt/OrderBook/issues/1) |
| piyush (148★) | C++ | with fix | 3.28 (flash-crash) | ~160 M/s | cached best-ask self-heal — re-seat to the next set level, not only on an empty side |
| fmstephe (474★) | Go | with fix | 2.48 (static) | — | crossing trades print at the maker price, not the midpoint |
| parity | Java | as shipped | 2.21 | — | RB-tree: TreeSet + fastutil id-map |
| dazzz1 | Java | with fix | 2.16 (static) | — | processOrder clears sell aggressor at own limit, not resting bid [#1](https://github.com/Dazzz1/warp-exchange/issues/1) |
| shivaganapathy | C++ | as shipped | 2.15 (normal) | — | two priority_queues + timestamp FIFO tiebreak (clean) |
| michaelliao | Java | as shipped | 2.09 | — | TreeMap RB-tree per side |
| ssuchichen | Go | with fix | 2.09 (normal) | — | cgo; 3 fixes — lost-trades, concurrent-map race, sell-side maker pricing [#1](https://github.com/ssuchichen/order-matching/issues/1) |
| maxe | C++ | with fix | 1.99 | — | deque-of-lists price-time + id map; partial-cancel no-op |
| coralme (56★) | Java | as shipped | 1.97 (flash-crash) | — |  |
| robaho | C++ | with fix | 1.90 (swing-25) | 10–22 M/s | execute at the resting (maker) price, not the aggressor's limit |
| geseq | Go | as shipped | 1.81 (swing-25) | 12.5–21M/s | a multi-level cross-through is fixed upstream — `RESOLVED_FINDINGS.md` |
| gocronx (84★) | Rust | as shipped | 1.77 (static) | ~17M/s |  |
| apex | Rust | with fix | 1.62 (static) | — | execute at the maker price, not the aggressor's limit |
| kartikeya | C++ | with fix | 1.61 | — | OrderIndex::erase backward-shift corruption fix [#1](https://github.com/Kartikeya2710/order-matching-engine/issues/1) |
| matchina | Rust | with fix | 1.60 (static) | — | taker-exhaustion guard — no phantom zero-quantity trades |
| charles | Java | with fix | 1.60 | — | compile + complete-fill abort fix [#1](https://github.com/CharlesMfouapon/limit-order-book/issues/1) |
| xingxing | Java | with fix | 1.58 | — | cancelOrder oidMap evict + compile fix [#1](https://github.com/crazyzym/xingxing-match-trading/issues/1) |
| rishib064 | Rust | as shipped | 1.55 | — | BTreeMap + VecDeque; adapter completes trade-emit + cancel |
| cryptonstudio | Go | as shipped | 1.47 | — | clean; a quote-locking pricing observation was investigated and dropped |
| Exchange-core (2556★) | Java/JVM | as shipped | 1.40 (flash-crash) | — | baseline; direct-access book, JNI per message |
| loom | Rust | as shipped | 1.39 (static) | — | FOK fillability checked per-resting-maker; multi-maker FOK wrongly killed [#1](https://github.com/AlphaGodzilla/loom/issues/1) |
| jiang | Java | with fix | 1.30 (swing-25) | — | 1-line `idMaps.remove(id)` so modify doesn't drop the order |
| sadhbh | C++ | with fix | 1.28 (static) | — | C++20 coroutines; exact-touch crossing + empty-book deref guard [#6](https://github.com/sadhbh-c0d3/cpp20-orderbook/issues/6) |
| magenta_mice | C++ | as shipped | 1.17 | — | std::map price → deque per side, native FAK/IOC |
| limitbook | Rust | with fix | 1.16 (static) | ~30 M/s | partial-fill write-back — decrement the resting maker |
| m5487 | Go | as shipped | 1.15 (swing-25) | ~2.6M/s | skiplist + disruptor |
| cjboxing | Java | with fix | 1.12 | — | filled order removed from PriceBucket but never orderMap; stale cancel acked [#1](https://github.com/cjBoxing/match/issues/1) (repo 404 since 2026-06-29) |
| dx1ngy | Java | with fix | 1.07 | — | match() prices fills at sell's price; maker-price fix [#1](https://github.com/dx1ngy/trading/issues/1) (resolved) |
| ironcrypto | Rust | with fix | 1.04 (6.2 normal) | — | held/no-license; adapter restores engine's removed cancel impl (faithfulness caveat); hold |
| yllvar | Rust | as shipped | 1.00 | — | clean matcher; off-scope settlement-Merkle odd-node duplication held (off scope) |
| shal | Go | with fix | 1.00 (static) | — | Engine.execute derives trade price from order.ID>other.ID, not resting maker [#1](https://github.com/shal/orderbook/issues/1) |
| jcwangjc | Java | as shipped | 0.96 | — | processMath copies taker's turnover into maker, off matched-output path [#1](https://github.com/jcwangjc/exchange-matching-engine/issues/1) |
| kodoh | C++ | with fix | 0.95 | — | crossing fills at maker price fix [#17](https://github.com/Kodoh/Orderbook/issues/17) |
| ffhan | Go | with fix | 0.89 | — | Cancel soft-flag fix [#4](https://github.com/ffhan/tome/issues/4) |
| kennethzhang | C++ | as shipped | 0.86 (static) | — | output-conforming — the adapter normalizes a taker-vs-maker price bug; [#1](https://github.com/kennethZhangML/TradingClientExchange/issues/1) |
| i25959341 (550★) | Go | with fix | 0.72 (swing-25) | >300k/s | per-side Volume() correct after a partial fill |
| jlob | Java/JNI | as shipped | 0.71 (static) | ~127 ns/op | L3 RB-tree, working JNI adapter |
| vdt | JavaScript | with fix | 0.71 | — | MIT; assert never imported → ReferenceError on side guard; hold |
| weblazy | Go | with fix | 0.71 (static) | — | GetFirst returns sentinel, GetLast nil-derefs on empty; guard fix [#1](https://github.com/weblazy/trade/issues/1) |
| mh2rashi | C++ | with fix | 0.70 (swing-40) | ~23k/s | 1-line `deleteOrder` list-corruption/crash fix |
| muzykantov | Go | with fix | 0.68 | — | MIT; OrderSide.Volume() overcounts after partial fill; hold |
| wezrule | C++ | with fix | 0.64 (static) | — | PoolAlloc deletes move-assignment → Market::operator= won't compile; compile fix [#1](https://github.com/wezrule/WezosTradingEngine/issues/1) |
| instrument_spot | Rust | as shipped | 0.60 (static) | — | depth maps pruned only on exact sum==0.0; f32 residual phantom level [#1](https://github.com/Andry-RALAMBOMANANTSOA/instrument_spot/issues/1) |
| danielgatis | Go | with fix | 0.58 (swing-25) | — | normalize the decimal price key — equal prices share one level |
| QuantCup (211★) | C++ | as shipped | 0.57 (flash-crash) | — | baseline; flat price-indexed array |
| gotrader (513★) | Go | with fix | 0.56 (swing-25) | 400k quote/s (net) | reject a modify of a fully-filled order (don't swallow-ack) |
| plutus | C++ | with fix | 0.53 | — | first-trade self-deadlock (mutex re-lock) fix [#1](https://github.com/bxptr/plutus/issues/1) |
| sohaibelkarmi | C++ | as shipped | 0.47 (7.4 static) | — | held/no-license; build workaround (4 absent sources); matcher core fine; 1 deep-cell crash [#6](https://github.com/sohaibelkarmi/High-Frequency-Trading-Simulator/issues/6) |
| fjmurcia | Rust | with fix | 0.43 | — | filled-maker id-index-remove fix [#2](https://github.com/fjmurcia/orderbook-rust/issues/2) |
| vllob | Julia | with fix | 0.43 (static) | — | AVL embed; made the size-walk limit-price-aware (was silently ignored) [#10](https://github.com/Renruize12306/VLLimitOrderBook.jl/issues/10) |
| mercury (notayessir) | Java | with fix | 0.39 | — | filled maker never evicted from id-index; stale cancel acked, NPEs [#1](https://github.com/notayessir/mercury-match-engine/issues/1) |
| shaunlwm | TypeScript | as shipped | 0.33 | — | updateOrder removes order twice, double-decrements level length, orphans siblings [#1](https://github.com/ShaunLWM/LimitOrderBook/issues/1) |
| pyme (133★) | Python | as shipped | 0.31 (swing-25) | ~150k/s | doubly-linked price levels (CPython embed) |
| dsirotkin | C++ | with fix | 0.31 (static) | — | cancel removes only the order, not the rest of the price level |
| viabtc | C | as shipped | 0.29 | — | skiplist + dict |
| omx | C | with fix | 0.29 | — | held/no-license; skiplist_release leaks node header+struct [#2](https://github.com/0xae/omx-engine/issues/2) |
| oceanbook | Go | with fix | 0.26 (flash-crash) | — | Depth writes quantity into the qty field, not the price field |
| silue | Python | with fix | 0.26 | — | held/no-license; get_pnl Decimal×None → crash on first cross [#1](https://github.com/silue-dev/limit-order-book-market-making/issues/1) |
| brprojects (186★) | C++ | as shipped | 0.23 (swing-25) | ~1.4M/s | uncached-height AVL (perf-only) |
| dyn4mik3 (409★) | Python | with fix | 0.23 (swing-25) | — | 1-line `get_price` → `get_price_list` crash fix |
| landakram | Rust | with fix | 0.22 (static) | — | clean; two unreachable observations dropped |
| lightning (754liam) | C++ | with fix | 0.22 (static) | — | matchAskLimit prices fills at aggressor's limit; maker-price fix [#1](https://github.com/754liam/Lightning/issues/1) |
| mkhoshkam | Go | with fix | 0.18 | — | heap Less ignores seq → FIFO fix (adapter, not filed) |
| rakuzen25 | C++ | with fix | 0.18 (flash-crash) | — | within-level FIFO fix (swap-with-last broke arrival order); uint16 ceiling residual; issues disabled upstream |
| trademacher | Java/JNI | as shipped | 0.15 (swing-25) | ~5M/s |  |
| OrderBook-rs (477★) | Rust | with fix | 0.13 (static) | latency-focused | partial-fill maker keeps FIFO priority (push_front, not re-queue to tail) |
| lirezap | Java | with fix | ~1.2–1.6 (0.11 static) | — | ship/ISC; FOK fills only best level, kills multi-level FOK [#1](https://github.com/lirezap/OMS/issues/1) |
| volt | Zig | as shipped | 0.10 | — | RB-tree + flat-array + hierarchical bitset, novel |
| vincurious | Java | with fix | 0.10 (static) | — | ask comparator orders highest-price-first; inversion fix [#4](https://github.com/vinCurious/OrderMatchingEngine/issues/4) |
| dgtony (453★) | Rust | with fix | 0.09 (static) | — | widen the id-gen range + amend can't leave the book crossed |
| shivamkachhadiya | C++ | with fix | 0.09 | — | best-first-sweep fix; GitLab filing pending |
| matchingo | Go | with fix | 0.08 (static) | — | UpdateVolume subtracts the consumed qty, not the remainder (depth audit); fixed upstream — `RESOLVED_FINDINGS.md` |
| php_matcher | PHP | as shipped | 0.08 | — | held/no-license; float→int key truncation merges sub-integer prices; issues disabled — unfileable |
| pantelwar (75★) | Go | with fix | 0.07 (static) | — | remove hot-path debug logging + fix the MarshalJSON sell-side bug |
| bexchange | Go | with fix | 0.07 | — | fill loop drops partial makers / phantom fills; dup of [#2](https://github.com/bhomnick/bexchange/issues/2) |
| lobrs | Rust | with fix | 0.07 | — | after cancel empties best level, best left None, never recomputed [#1](https://github.com/rafalpiotrowski/lob-rs/issues/1) |
| yihuang | Python | as shipped | 0.06 (static) | — | cancel_order's unguarded self.levels[price] KeyErrors after fill empties level [#1](https://github.com/yihuang/pyorderbook/issues/1) (resolved) |
| jeog | C++ | as shipped | 0.05 (flash-crash) | — | flat directly-indexed price vector |
| jxm35 | C++ | with fix | 0.05 (static) | 14 M/s | drop the redundant hand-splice (restores level accounting) + emit trade reports |
| turbo | Java | with fix | 0.05 (static; ~0.6–0.9 typical) | — | ship/Apache-2.0; native IOC rests residual on empty book [#1](https://github.com/sluggard6/turbo/issues/1) |
| aodr3w | Rust | with fix | 0.05 (static) | — | filled maker list-unlinked only; ids leak, zero-alloc arena OOMs [#1](https://github.com/aodr3w/zero-alloc-lob/issues/1) |
| fractal | Java | with fix | 0.05 (static) | — | JNI; crossing now fills at the maker price, not the lowest offer [#1](https://github.com/FractalFinTech/OrderBook/issues/1) |
| jpalounek | Python | with fix | 0.04 | — | AVL snapshot-iteration fix; sweep's sole diverger [#1](https://github.com/JPalounek/order-book/issues/1) |
| baoyingwang | Java | as shipped | 0.04 (static; ~1.6 typical) | — | MIT; FIFO-tiebreak subtraction overflows >24.85-day age; 6 deep-cell crashes; hold |
| Liquibook (1479★) | C++ | as shipped | 0.03 (static) | — | baseline; price-keyed multimap of lists (native IOC residual handled adapter-side, #43) |
| philipgreat (98★) | Rust | with fix | 0.03 (static) | ~8 ns/order | 3 cancel/modify-path correctness fixes |
| cointossx (122★) | Java/JNI | with fix | 0.03 (static) | — | 2 fixes: B+Tree destructive `firstKey` + `AddOrderPreProcessor` wrong-side compare |
| betting_exchange | Scala | as shipped | ~0.03 (seed-23) | — | held/no-license; cancelled bet id retained (betsIds never pruned); seed-23 only; hold |
| dhyey | C++ | as shipped | 0.03 (static) | — | delete_order order_map[id] inserts then derefs null Order* → SIGSEGV [#1](https://github.com/Dhyey-Mehta/order-book/issues/1) |
| rabbittrix | Rust | with fix | 0.02 | — | bid sort-key + cancel-stub fix [#1](https://github.com/rabbittrix/Ultra-Low-Latency-FX-eTrading-Platform/issues/1) |
| lll | C++ | with fix | 0.02 (static; ~0.4–2 typical) | — | held/no-license; modify_order_in_map no-op; hold (unfiled) |
| rhodey | JavaScript | as shipped | ~0.02–0.12 (swings) | — | MIT; clean |
| prystupa | Scala | with fix | 0.02 (static) | — | held/no-license; FastList.removeInto drops tail-removed-then-reappended element [#6](https://github.com/prystupa/scala-cucumber-matching-engine/issues/6) |
| mkxzy | Java | as shipped | 0.02 (static) | — | clean; held for no license |
| auralshin | Rust | as shipped | 0.01 (static) | — |  |
| cspooner | Rust | with fix | very slow (static) | — | conserve quantity on a partial fill (+ 4 related fixes) |
| pgellert (65★) | Rust | with fix | very slow (swing-40) | — | don't drop the popped order + correct the stale price-bounds |
| mansoor (64★) | C++ | with fix | very slow (normal) | >20 M/s | bounds-check the price array — no OOB on wide swings |
| luo4neck | C++ | with fix | very slow (static) | — | match best-price-first (price-time priority) |
| pyxchange | C++ | with fix | very slow (static) | ~100k/s | monotonic (price,seq) key — no dropped same-tick same-price orders |
| darkpool | C++ | with fix | very slow (static) | — | report the execution at the maker price |
| fasenderos (202★) | TypeScript | as shipped | very slow (static) | >300k/s | libnode embed |
| lobster (172★) | Rust | as shipped | very slow (swing-40) | — | BTreeMap + arena + id-index |
| techieboy (58★) | Rust | with fix | very slow (swing-25) | ~10 µs/match | 2 fixes: spurious zero-qty fills + stale best-bid/ask |
| lightning (68★) | Go | with fix | very slow (static) | — | skiplist multi-level predecessor fix — no lost resting orders |
| ridulfo (69★) | Python | with fix | very slow (static) | ~400k/s | consistent total order in __lt__ — no priority inversion / lost cancels |
| khrapovs | Python | with fix | very slow (pure Python) | — | MIT; orders_by_expiration not pruned on fill [#25](https://github.com/khrapovs/OrderBookMatchingEngine/issues/25) |
| dabrowdev | TypeScript | as shipped | very slow (static) | — | MIT; O(n²) cancel/sweep, OrderQueue.remove reindexes level; Codeberg — filing TODO |
| ms_engine | TypeScript | as shipped | very slow | — | no-liquidity market order rests remainder at sentinel −1; market-path only, held |
| lsamber | Java | as shipped | very slow | — | same-price FIFO breaks on LocalDateTime.now() collision in non-stable PriorityQueue [#1](https://github.com/LS-Amber/financial-trading-system/issues/1) |
| m15102785298 | Java | as shipped | very slow | — | binary-search LinkedList via get(mid) → O(n² log n) deep-book build [#1](https://github.com/15102785298/Matching-algorithm/issues/1) |

*very slow* = the engine did not clear the workload within the time budget in its weakest scenario (worst-case throughput below ~0.01 M/s).

Conformance is a correctness property, not a quality ranking; these engines span every common book architecture (flat arrays, hierarchical bitmaps, RB / AVL / B+ trees, skip lists, intrusive FIFO queues) across 20+ languages. `CORRECTNESS_FINDINGS.md` carries a one-line finding and the filed-issue link for each (the issue documents the patch).
