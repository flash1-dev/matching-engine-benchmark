# Consensus-conforming engines

Companion to [`README.md`](README.md) (which shows the top 10 by worst-case throughput). A one-line finding and the filed-issue link per engine are in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) (the linked issue carries the full mechanism and patch); the pre-run conformance gate is in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

These **160** engines (110 of them with our suggested fix) reproduce the byte-identical consensus on both the report stream and the book state across 100 random workload seeds — **1 billion+ order messages** each — and pass the pre-run conformance gate. **as shipped** = conforms unmodified with no known correctness defect. **with fix** = reproduces the consensus after the minimal documented patch — whether the defect diverges the canonical workload or is *latent* (reachable only off-workload or through the book-state audit); the filed issue carries the patch. The table spans every common book architecture and 16 source languages (the full 247-engine survey spans 20+). Every row describes the engine at its pinned commit in [`SNAPSHOTS.md`](SNAPSHOTS.md) — a reproducible snapshot, **not a judgment of current code or engineering quality**.

The **Worst-case M/s** column is each engine's lowest throughput across the five scenarios (weakest regime, seed 23, Graviton4 / Neoverse-V2, `-O3 -march=native`), measured **in isolation on clean cores** at the median of 10 trials. A parenthesised scenario names the weakest regime where recorded; the broader survey rows give the worst-of-five as a single figure.

**‡** = authored by a professional trading-industry engineer — a **personal side project with no commercial intent, not their employer's work**, except where the Notes explicitly label an official vendor/org repo. Affiliations are as the authors publicly state them, not independently verified by us. · **★** = repository has 50+ GitHub stars. **Published figure** = the project's own advertised number under its own workload, hardware, and definition — shown as context, not directly comparable to this harness's worst-case. The industry-authored subset is broken out in [`INDUSTRY_AUTHORED_ENGINES.md`](INDUSTRY_AUTHORED_ENGINES.md).

| Engine | Language | Conformance | Worst-case M/s | Published figure | Notes |
|:--|:--|:--|--:|:--|:--|
| FlashOne | C++ | as shipped | 33.20 (normal) | — | reference target |
| e820 / weekend-orderbook | C | with fix | 8.19 | — | singly-linked orphan + aggressor-price fix [#1](https://github.com/oldfifteenpoundy/weekend-orderbook/issues/1) |
| geseq/cpp-orderbook | C++ | as shipped | 7.94 (swing-25) | — | author-contributed C++ port of geseq/orderbook |
| melin | Rust | with fix | 7.86 | — | BSL-1.1; stop-trigger cascade single-pass [#2](https://github.com/melin-engine/melin/issues/2) |
| CppTrader (1041★) | C++ | as shipped | 7.26 (normal) | ~7.2M upd/s | a `ModifyOrder` defect off the canonical path is fixed upstream — `RESOLVED_FINDINGS.md` [#42](https://github.com/chronoxor/CppTrader/issues/42) |
| raymondshe (56★) | Rust | with fix | 7.20 | — | MIT-Apache; phantom zero-qty match corrupts next order's id [#1](https://github.com/raymondshe/matchengine-raft/issues/1) |
| Kautenja (309★) | C++ | with fix | 6.88 (normal) | — | reject a duplicate live order-id (no self-linked FIFO / UAF) [#4](https://github.com/Kautenja/limit-order-book/issues/4) |
| ndfex ‡ | C++ | as shipped | 6.825 (swing-25) | — | std::map RB-tree book (clean); author: Matthew Belcher (ex-Citadel Securities, 17y HFT) |
| matchcore | Rust | with fix | 6.58 | — | marketable limit passes None → sweeps like market order, pays through own limit [#167](https://github.com/minyukim/matchcore/issues/167) |
| chronex | C++ | with fix | 6.47 | — | MIT; FOK/AON makers fill at aggressor price [#1](https://github.com/OsamaAhmad00/ChroneX/issues/1) |
| yashkukrecha ‡ | C++ | as shipped | 6.26 (normal) | — | two priority_queues + timestamp FIFO tiebreak (clean; fastest pro-wave conformer); author: incoming at Jump Trading |
| lobsim | C++ | as shipped | 6.07 | — | flat_hash_map + Boost intrusive list + max-heaps |
| asthamishra | Rust | with fix | 5.60 (flash-crash) | — | bounds-check the tick array — no dropped orders above the ceiling [#1](https://github.com/AsthaMishra/matching-engine/issues/1) |
| llc993 (154★) | Rust | as shipped | 5.43 (swing-40) | ~7.2M/s | BTreeMap + slab pool + intrusive time-queue (exchange-core-inspired) |
| newbigdeng | C++ | with fix | 5.38 | — | won't compile — flushQueue brace imbalance + two stale call sites [#1](https://github.com/newbigdeng/TradeSystem/issues/1) |
| johannestampere | C++ | with fix | 5.31 | — | get_best_price() returns 0 for empty book (latent; all calls guarded today) |
| bozoslav | C++ | as shipped | 5.01 | — | per-side price array + slab pool + id index, native IOC/FOK/modify |
| hroptatyr/clob | C | as shipped | 4.73 (normal) | ~6M/s | b+tree CLOB, `_Decimal64` (no patch) |
| onewhitedevil | C++ | with fix | 4.73 | — | MIT; cancel never frees slab slot → bad_alloc [#1](https://github.com/1WHITE-DEVIL/lob-matching-engine/issues/1) |
| slmolenaar (264★) | C++ | with fix | 4.50 | — | CancelOrder swap-and-pop FIFO fix [#3](https://github.com/SLMolenaar/orderbook-simulator-cpp/issues/3) |
| forever803 | C++ | as shipped | 4.33 | — | no-license; conforming; the matcher ships inside a demo/example harness |
| ranjan2829 (131★) | C++ | with fix | 4.07 | — | 4 memory-safety defects fix [#3](https://github.com/ranjan2829/High-Frequency-Trading-Exchange-Engine/issues/3) |
| mercury | C++ | as shipped | 3.94 (normal) | 3.2M/s | abseil b-tree |
| rust_ob | Rust | with fix | 3.73 (static) | — | Decimal::MAX sentinel overflows rust_decimal → panic; unreachable through harness [#1](https://github.com/toyota-corolla0/rust_ob/issues/1) |
| microexchange (62★) | C++ | as shipped | 3.62 (flash-crash) | 2.24M/s | array + bitmap |
| faulaire | C++ | as shipped | 3.62 | — | Boost.MultiIndex: hashed id + ordered price |
| daniele ‡ | C++ | with fix | 3.60 (static) | — | fill-report reads a maker freed in the same fill (matching is correct); [#1](https://github.com/Daniele122898/Trading-Engine/issues/1); author: an Optiver engineer |
| nanobook | Rust | as shipped | 3.52 | — | MIT; clean |
| Tzadiko (307★) | C++ | with fix | 3.39 (flash-crash) | — | IOC self-deadlock; two-site lock-wrapper fix [#11](https://github.com/Tzadiko/Orderbook/issues/11) + [#12](https://github.com/Tzadiko/Orderbook/issues/12) |
| timothewt | C++ | with fix | 3.34 | — | MIT; prev/next both shared_ptr → reference-cycle leak [#1](https://github.com/timothewt/OrderBook/issues/1) |
| piyush (148★) | C++ | with fix | 3.28 (flash-crash) | ~160 M/s | cached best-ask self-heal — re-seat to the next set level, not only on an empty side [#9](https://github.com/PIYUSH-KUMAR1809/order-matching-engine/issues/9) |
| serum ‡ | Rust | as shipped | 2.625 (static) | — | de-chained Solana CLOB (Project Serum, the original Solana on-chain order book); clean |
| fmstephe (474★) | Go | with fix | 2.48 (static) | — | crossing trades print at the maker price, not the midpoint [#11](https://github.com/fmstephe/matching_engine/issues/11) |
| parity (502★) ‡ | Java | as shipped | 2.21 | — | RB-tree: TreeSet + fastutil id-map; Parity Trading's own engine (org bio: 'Open source trading technologies') |
| dazzz1 | Java | with fix | 2.16 (static) | — | processOrder clears sell aggressor at own limit, not resting bid [#1](https://github.com/Dazzz1/warp-exchange/issues/1) |
| shivaganapathy ‡ | C++ | as shipped | 2.15 (normal) | — | two priority_queues + timestamp FIFO tiebreak (clean); author: an IMC engineer |
| manifest ‡ | Rust | as shipped | 2.145 (static) | — | de-chained production Solana CLOB (Manifest); clean |
| michaelliao (58★) | Java | as shipped | 2.09 | — | TreeMap RB-tree per side |
| ssuchichen | Go | with fix | 2.09 (normal) | — | cgo; 3 fixes — lost-trades, concurrent-map race, sell-side maker pricing [#1](https://github.com/ssuchichen/order-matching/issues/1) |
| maxe (66★) | C++ | with fix | 1.99 | — | deque-of-lists price-time + id map; partial-cancel no-op |
| coralme (56★) ‡ | Java | as shipped | 1.97 (flash-crash) | — | CoralBlocks' own open-source engine (trading-tech vendor) |
| robaho | C++ | with fix | 1.90 (swing-25) | 10–22 M/s | execute at the resting (maker) price, not the aggressor's limit [#2](https://github.com/robaho/cpp_orderbook/issues/2) |
| geseq | Go | as shipped | 1.81 (swing-25) | 12.5–21M/s | a multi-level cross-through is fixed upstream — `RESOLVED_FINDINGS.md` [#25](https://github.com/geseq/orderbook/issues/25) |
| gocronx (84★) | Rust | as shipped | 1.77 (static) | ~17M/s |  |
| robdev ‡ | Rust | with fix | 1.76 (static) | — | clear the emptied price level on cancel, return the real cancel result, and kill the IOC residual — all latent as-shipped (match path immune; the stale best_price is caught by the gate's state audit) [#1](https://github.com/rob-DEV/match-engine/issues/1); author: a CME Group engineer |
| stocksharp (10236★) ‡ | C# | with fix | 1.64 (swing-25) | — | same-price orders could match out of arrival order (Dictionary enumeration replaces FIFO); conforms with the documented one-line fix; author: StockSharp / trading-tech vendor; [#681](https://github.com/StockSharp/StockSharp/issues/681) |
| apex | Rust | with fix | 1.62 (static) | — | execute at the maker price, not the aggressor's limit [#3](https://github.com/crypto-zero/apex-engine/issues/3) |
| kartikeya | C++ | with fix | 1.61 | — | OrderIndex::erase backward-shift corruption fix [#1](https://github.com/Kartikeya2710/order-matching-engine/issues/1) |
| matchina ‡ | Rust | with fix | 1.60 (static) | — | taker-exhaustion guard — no phantom zero-quantity trades; fixed upstream — `RESOLVED_FINDINGS.md`; author: at GSR (crypto market maker) [#3](https://github.com/fran0x/matchina/issues/3) |
| charles | Java | with fix | 1.60 | — | compile + complete-fill abort fix [#1](https://github.com/CharlesMfouapon/limit-order-book/issues/1) |
| harsh4786 | Rust | as shipped | 1.60 (static) | — | a Solana agnostic-order-book run off-chain in-process; clean, full consensus |
| xingxing | Java | with fix | 1.58 | — | cancelOrder oidMap evict + compile fix [#1](https://github.com/crazyzym/xingxing-match-trading/issues/1) |
| rishib064 | Rust | with fix | 1.55 | — | matcher emits no trades as shipped (executions decrement qty only); adapter completes trade-emit + cancel [#1](https://github.com/RishiB064/Rust-Limit-Order-Book/issues/1) |
| phoenix ‡ | Rust | as shipped | 1.5 (static) | — | de-chained production Solana CLOB (Ellipsis Labs; founders ex-Jane Street/Citadel); clean |
| tembolo ‡ | C | with fix | 1.475 (swing-25) | — | two capacity ceilings (8192-order pool silent-drop + 512 price-level abort) [#1](https://github.com/tembolo1284/matching-engine-c/issues/1); author: a quantitative developer at Tradeweb |
| cryptonstudio | Go | as shipped | 1.47 | — | clean; a quote-locking pricing observation was investigated and dropped |
| Exchange-core (2556★) | Java/JVM | as shipped | 1.40 (flash-crash) | — | baseline; direct-access book, JNI per message |
| loom | Rust | with fix | 1.39 (static) | — | check FOK fillability against total reachable quantity, not one maker at a time — a multi-maker-fillable FOK fills [#1](https://github.com/AlphaGodzilla/loom/issues/1) |
| jiang | Java | with fix | 1.30 (swing-25) | — | 1-line `idMaps.remove(id)` so modify doesn't drop the order [#3](https://github.com/JiangYongKang/FastMatchingEngine/issues/3) |
| sadhbh | C++ | with fix | 1.28 (static) | — | C++20 coroutines; exact-touch crossing + empty-book deref guard [#6](https://github.com/sadhbh-c0d3/cpp20-orderbook/issues/6) |
| koral ‡ | C++ | as shipped | 1.255 (normal) | — | FIX exchange (clean; thread-affinity plumbing only); author: a Coinbase software-engineering intern |
| magenta_mice | C++ | as shipped | 1.17 | — | std::map price → deque per side, native FAK/IOC |
| limitbook | Rust | with fix | 1.16 (static) | ~30 M/s | partial-fill write-back — decrement the resting maker [#1](https://github.com/solarpx/limitbook/issues/1) |
| m5487 | Go | as shipped | 1.15 (swing-25) | ~2.6M/s | skiplist + disruptor |
| cjboxing | Java | with fix | 1.12 | — | filled order removed from PriceBucket but never orderMap; stale cancel acked [#1](https://github.com/cjBoxing/match/issues/1) (repo 404 since 2026-06-29) |
| dx1ngy | Java | with fix | 1.07 | — | match() prices fills at sell's price; maker-price fix [#1](https://github.com/dx1ngy/trading/issues/1) (resolved) |
| ironcrypto ‡ | Rust | with fix | 1.04 (6.2 normal) | — | no-license; adapter restores engine's removed cancel impl (faithfulness caveat); author: self-described 'TradFi/DeFi Quant' |
| yllvar | Rust | as shipped | 1.00 | — | clean matcher; off-scope settlement-Merkle odd-node duplication (off scope) |
| shal | Go | with fix | 1.00 (static) | — | Engine.execute derives trade price from order.ID>other.ID, not resting maker [#1](https://github.com/shal/orderbook/issues/1) |
| jcwangjc | Java | with fix | 0.96 | — | accumulate the maker's turnover from its own running total, not the taker's (off the matched-output path) [#1](https://github.com/jcwangjc/exchange-matching-engine/issues/1) |
| kodoh (76★) | C++ | with fix | 0.95 | — | crossing fills at maker price fix [#17](https://github.com/Kodoh/Orderbook/issues/17) |
| trusted ‡ | Rust | with fix | 0.925 (static) | — | latent bid-side market-order double-subtract underflow [#9](https://github.com/JunbeomL22/trusted/issues/9); author: a KRX market-maker at IBK Securities |
| ffhan | Go | with fix | 0.89 | — | Cancel soft-flag fix [#4](https://github.com/ffhan/tome/issues/4) |
| kennethzhang ‡ | C++ | with fix | 0.86 (static) | — | price the limit-vs-limit cross at the resting maker, not the taker (the adapter normalizes it today) [#1](https://github.com/kennethZhangML/TradingClientExchange/issues/1); author: a Squarepoint quant researcher |
| javalob ‡ | Java | as shipped | 0.86 (swing-40) | — | teaching LOB (clean); author: Ash Booth (JPMorgan) |
| swirly ‡ | Java | as shipped | 0.79 (swing-40) | — | clean — native revise changes only lots, so modify = cancel+reinsert per contract; author: a trading-systems developer; co-founder of Reactive Markets |
| i25959341 (550★) | Go | with fix | 0.72 (swing-25) | >300k/s | per-side Volume() correct after a partial fill |
| jlob | Java/JNI | as shipped | 0.71 (static) | ~127 ns/op | L3 RB-tree, working JNI adapter |
| vdt | JavaScript | with fix | 0.71 | — | MIT; assert never imported → ReferenceError on side guard |
| weblazy | Go | with fix | 0.71 (static) | — | GetFirst returns sentinel, GetLast nil-derefs on empty; guard fix [#1](https://github.com/weblazy/trade/issues/1) |
| mh2rashi | C++ | with fix | 0.70 (swing-40) | ~23k/s | 1-line `deleteOrder` list-corruption/crash fix [#4](https://github.com/mh2rashi/Trading-Engine/issues/4) |
| muzykantov | Go | with fix | 0.68 | — | MIT; OrderSide.Volume() overcounts after partial fill |
| wezrule | C++ | with fix | 0.64 (static) | — | PoolAlloc deletes move-assignment → Market::operator= won't compile; compile fix [#1](https://github.com/wezrule/WezosTradingEngine/issues/1) |
| instrument_spot | Rust | with fix | 0.60 (static) | — | prune a depth level on "no orders left", not an exact f32 sum==0.0 — no phantom level under fractional quantities [#1](https://github.com/Andry-RALAMBOMANANTSOA/instrument_spot/issues/1) |
| danielgatis | Go | with fix | 0.58 (swing-25) | — | normalize the decimal price key — equal prices share one level [#2](https://github.com/danielgatis/go-orderbook/issues/2) |
| QuantCup (211★) | C++ | as shipped | 0.57 (flash-crash) | — | baseline; flat price-indexed array |
| gotrader (513★) | Go | with fix | 0.56 (swing-25) | 400k quote/s (net) | reject a modify of a fully-filled order (don't swallow-ack) [#23](https://github.com/robaho/go-trader/issues/23) |
| plutus | C++ | with fix | 0.53 | — | first-trade self-deadlock (mutex re-lock) fix [#1](https://github.com/bxptr/plutus/issues/1) |
| sohaibelkarmi | C++ | as shipped | 0.47 (7.4 static) | — | no-license; build workaround (4 absent sources); matcher core fine; 1 deep-cell crash [#6](https://github.com/sohaibelkarmi/High-Frequency-Trading-Simulator/issues/6) |
| omerhalid | C++ | with fix | 0.46 (static) | — | no-license; latent partial-fill depth over-count (`total_quantity_` not decremented) — report stream correct, fails the state audit [#3](https://github.com/omerhalid/Real-Time-Market-Data-Feed-Handler-and-Order-Matching-Engine/issues/3) |
| fjmurcia | Rust | with fix | 0.43 | — | filled-maker id-index-remove fix [#2](https://github.com/fjmurcia/orderbook-rust/issues/2) |
| vllob | Julia | with fix | 0.43 (static) | — | AVL embed; made the size-walk limit-price-aware (was silently ignored) [#10](https://github.com/Renruize12306/VLLimitOrderBook.jl/issues/10) |
| mercury (notayessir) | Java | with fix | 0.39 | — | filled maker never evicted from id-index; stale cancel acked, NPEs [#1](https://github.com/notayessir/mercury-match-engine/issues/1) |
| circus | C# | with fix | 0.39 (swing-25) | — | reusing a completed order id crashed; partial-fill residual mishandled; fixed; [#1](https://github.com/seanoflynn/circus/issues/1) |
| damian ‡ | Kotlin | as shipped | 0.38 (static) | — | clean; author: Damian Howard (20y at a bank) |
| shaunlwm | TypeScript | with fix | 0.33 | — | remove the repriced order once (via removeOrderById) so a shared-level modify doesn't orphan its siblings [#1](https://github.com/ShaunLWM/LimitOrderBook/issues/1) |
| dsirotkin | C++ | with fix | 0.31 (static) | — | cancel removes only the order, not the rest of the price level |
| pyob ‡ | Python | with fix | 0.30 (swing-25) | — | deque IndexError on a full fill + stale best_price after cancel [#1](https://github.com/wegar-2/pyob/issues/1); author: an FX e-trading quant at mBank |
| viabtc (2784★) | C | as shipped | 0.29 | — | skiplist + dict |
| omx | C | with fix | 0.29 | — | no-license; skiplist_release leaks node header+struct [#2](https://github.com/0xae/omx-engine/issues/2) |
| joaquinbejar ‡ | Rust | with fix | 0.29 (static) | — | Book::replace crossing modify never re-matches → rests a crossed book [#59](https://github.com/joaquinbejar/hft-clob-core/issues/59); author: a quant developer at Capital Delta |
| oceanbook ‡ | Go | with fix | 0.26 (flash-crash) | — | Depth writes quantity into the qty field, not the price field; author: self-described HFT developer (bio 'HFT / C++ / Go'; @spectra-fund) [#44](https://github.com/draveness/oceanbook/issues/44) |
| silue | Python | with fix | 0.26 | — | no-license; get_pnl Decimal×None → crash on first cross [#1](https://github.com/silue-dev/limit-order-book-market-making/issues/1) |
| brprojects (186★) | C++ | as shipped | 0.23 (swing-25) | ~1.4M/s | uncached-height AVL (perf-only) |
| dyn4mik3 (409★) | Python | with fix | 0.23 (swing-25) | — | 1-line `get_price` → `get_price_list` crash fix [#22](https://github.com/dyn4mik3/OrderBook/issues/22) |
| landakram | Rust | with fix | 0.22 (static) | — | clean; two unreachable observations dropped |
| lightning (754liam) | C++ | with fix | 0.22 (static) | — | matchAskLimit prices fills at aggressor's limit; maker-price fix [#1](https://github.com/754liam/Lightning/issues/1) |
| mkhoshkam | Go | with fix | 0.18 | — | heap Less ignores seq → FIFO fix (adapter) [#10](https://github.com/mkhoshkam/orderbook/issues/10) |
| rakuzen25 ‡ | C++ | with fix | 0.18 (flash-crash) | — | within-level FIFO fix (swap-with-last broke arrival order); uint16 ceiling residual; issues disabled upstream; author: an Optiver intern |
| sculd ‡ | Python | with fix | 0.18 (swing-25) | — | guard unknown-id cancel/status against KeyError and skip cancelled heads in _get_best_price — latent as-shipped (matching path unaffected) [#1](https://github.com/sculd/orderbook_practice_python/issues/1); author: a quant/developer who has worked at Two Sigma |
| trademacher ‡ | Java/JNI | as shipped | 0.15 (swing-25) | ~5M/s | TradeMatcher's own matching engine (trading-tech vendor) |
| OrderBook-rs (477★) ‡ | Rust | with fix | 0.13 (static) | latency-focused | partial-fill maker keeps FIFO priority (push_front, not re-queue to tail); fixed upstream — `RESOLVED_FINDINGS.md`; author: a quant developer at Capital Delta [#88](https://github.com/joaquinbejar/OrderBook-rs/issues/88) |
| lirezap | Java | with fix | ~1.2–1.6 (0.11 static) | — | ISC; FOK fills only best level, kills multi-level FOK [#1](https://github.com/lirezap/OMS/issues/1) |
| dydx ‡ | Go | as shipped | 0.11 (swing-25) | — | de-chained v4 memclob (accessor-only); author: Antonio Juliano (dYdX founder) |
| volt | Zig | as shipped | 0.10 | — | RB-tree + flat-array + hierarchical bitset, novel |
| vincurious | Java | with fix | 0.10 (static) | — | ask comparator orders highest-price-first; inversion fix [#4](https://github.com/vinCurious/OrderMatchingEngine/issues/4) |
| dgtony (453★) | Rust | with fix | 0.09 (static) | — | widen the id-gen range + amend can't leave the book crossed [#9](https://github.com/dgtony/orderbook-rs/issues/9) |
| shivamkachhadiya | C++ | with fix | 0.09 | — | best-first-sweep fix; GitLab filing pending |
| qa-rs ‡ | Rust | with fix | 0.09 (static) | — | OrderQueue lazy-deletion — same-id reinsert leaves a stale heap entry [#1](https://github.com/yutiansut/qa-rs/issues/1), `get_depth` over-counts a plain cancel until swept [#2](https://github.com/yutiansut/qa-rs/issues/2); + 5 latent `Orderbook` match-loop bugs off the limit-only workload: 1000-id recycle drops orders [#3](https://github.com/yutiansut/qa-rs/issues/3), market remainder rested not killed [#4](https://github.com/yutiansut/qa-rs/issues/4), amend skips the crossing check [#5](https://github.com/yutiansut/qa-rs/issues/5), NaN price passes validation [#6](https://github.com/yutiansut/qa-rs/issues/6), per-order sweep recursion overflows the stack [#7](https://github.com/yutiansut/qa-rs/issues/7); author: a private-fund manager (Shanghai Binghao) |
| matchingo | Go | with fix | 0.08 (static) | — | UpdateVolume subtracts the consumed qty, not the remainder (depth audit); fixed upstream — `RESOLVED_FINDINGS.md` [#1](https://github.com/GOnevo/matchingo/issues/1) |
| php_matcher | PHP | with fix | 0.08 | — | no-license; key price levels by the integer tick so distinct sub-integer prices don't merge; issues disabled — unfileable |
| vega ‡ | Go | as shipped | 0.08 (static) | — | accessor-only; author: Barney Mannerings (designed a London Stock Exchange matcher; Vega Protocol) |
| pantelwar (75★) | Go | with fix | 0.07 (static) | — | remove hot-path debug logging + fix the MarshalJSON sell-side bug [#26](https://github.com/Pantelwar/matching-engine/issues/26) |
| bexchange | Go | with fix | 0.07 | — | fill loop drops partial makers / phantom fills; dup of [#2](https://github.com/bhomnick/bexchange/issues/2) |
| lobrs | Rust | with fix | 0.07 | — | after cancel empties best level, best left None, never recomputed [#1](https://github.com/rafalpiotrowski/lob-rs/issues/1) |
| yihuang | Python | with fix | 0.06 (static) | — | look up the level with .get() so a too-late cancel rejects instead of KeyError-ing after a fill emptied it [#1](https://github.com/yihuang/pyorderbook/issues/1) (resolved) |
| jeog | C++ | as shipped | 0.05 (flash-crash) | — | flat directly-indexed price vector |
| jxm35 | C++ | with fix | 0.05 (static) | 14 M/s | drop the redundant hand-splice (restores level accounting) + emit trade reports [#1](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1) |
| turbo | Java | with fix | 0.05 (static; ~0.6–0.9 typical) | — | Apache-2.0; native IOC rests residual on empty book; a deep same-price sweep also overflows the JVM stack via unbounded `doOrder` recursion [#1](https://github.com/sluggard6/turbo/issues/1), [#2](https://github.com/sluggard6/turbo/issues/2) |
| aodr3w | Rust | with fix | 0.05 (static) | — | filled maker list-unlinked only; ids leak, zero-alloc arena OOMs [#1](https://github.com/aodr3w/zero-alloc-lob/issues/1) |
| fractal | Java | with fix | 0.05 (static) | — | JNI; crossing now fills at the maker price, not the lowest offer [#1](https://github.com/FractalFinTech/OrderBook/issues/1) |
| jpalounek | Python | with fix | 0.04 | — | AVL snapshot-iteration fix; sweep's sole diverger [#1](https://github.com/JPalounek/order-book/issues/1) |
| baoyingwang | Java | with fix | 0.04 (static; ~1.6 typical) | — | MIT; use Long.compare in the FIFO tiebreak so a >24.85-day order age can't overflow the (int) cast [#4](https://github.com/baoyingwang/OrderBook/issues/4) |
| Liquibook (1479★) | C++ | with fix | 0.03 (static) | — | baseline; native IOC residual rests instead of cancelling — corrected adapter-side [#43](https://github.com/enewhuis/liquibook/issues/43) |
| philipgreat (98★) | Rust | with fix | 0.03 (static) | ~8 ns/order | 3 cancel/modify-path correctness fixes [#1](https://github.com/philipgreat/lighting-match-engine-core/issues/1) |
| cointossx (122★) | Java/JNI | with fix | 0.03 (static) | — | 2 fixes: B+Tree destructive `firstKey` + `AddOrderPreProcessor` wrong-side compare [#10](https://github.com/dharmeshsing/CoinTossX/issues/10) |
| betting_exchange | Scala | with fix | ~0.03 (seed-23) | — | no-license; cancelled bet id retained (betsIds never pruned); latent; seed-23 only |
| dhyey | C++ | with fix | 0.03 (static) | — | look up delete_order with find(), not operator[], so a cancel of a non-resting order rejects instead of SIGSEGV-ing [#1](https://github.com/Dhyey-Mehta/order-book/issues/1); and pop the emptied level in `Limit::remove_order` so a cancel at the best price doesn't leave a stale zero-volume top-of-book (book-state audit) [#2](https://github.com/Dhyey-Mehta/order-book/issues/2) |
| rabbittrix | Rust | with fix | 0.02 | — | bid sort-key + cancel-stub fix [#1](https://github.com/rabbittrix/Ultra-Low-Latency-FX-eTrading-Platform/issues/1) |
| rhodey | JavaScript | as shipped | ~0.02–0.12 (swings) | — | MIT; clean |
| mkxzy | Java | as shipped | 0.02 (static) | — | clean; no license |
| auralshin | Rust | as shipped | 0.01 (static) | — |  |
| zorrofix ‡ | C++ | with fix | 0.01 (static) | — | sweep loop never checks whether the aggressor has closed → execute(0)→NAN→abort [#12](https://github.com/dsec-capital/zorro-fix/issues/12); DSEC Capital's own repo (org bio: 'Algo Trading Tech Shop') |
| ghosh (677★) ‡ | C++ | with fix | very slow | — | MIT; flat 256-slot price index shared by both sides had no collision handling → cross-side bucket merge crash; + a 1,048,576 order-id cap; both fixed; author: a low-latency trading-systems developer and Packt author; [#9](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP/issues/9) |
| cspooner | Rust | with fix | very slow (static) | — | conserve quantity on a partial fill (+ 4 related fixes) [#14](https://github.com/christian-spooner/trading-server/issues/14) |
| pgellert (65★) | Rust | with fix | very slow (swing-40) | — | don't drop the popped order + correct the stale price-bounds [#2](https://github.com/pgellert/matching-engine/issues/2) |
| mansoor (64★) | C++ | with fix | very slow (normal) | >20 M/s | bounds-check the price array — no OOB on wide swings [#3](https://github.com/mansoor-mamnoon/limit-order-book/issues/3) |
| luo4neck | C++ | with fix | very slow (static) | — | match best-price-first (price-time priority) [#3](https://github.com/luo4neck/MatchingEngine/issues/3) |
| pyxchange | C++ | with fix | very slow (static) | ~100k/s | monotonic (price,seq) key — no dropped same-tick same-price orders |
| darkpool | C++ | with fix | very slow (static) | — | report the execution at the maker price [#1](https://github.com/dendisuhubdy/dark_pool/issues/1) |
| fasenderos (202★) | TypeScript | as shipped | very slow (static) | >300k/s | libnode embed |
| lobster (172★) | Rust | as shipped | very slow (swing-40) | — | BTreeMap + arena + id-index |
| techieboy (58★) | Rust | with fix | very slow (swing-25) | ~10 µs/match | 2 fixes: spurious zero-qty fills + stale best-bid/ask [#1](https://github.com/TechieBoy/rust-orderbook/issues/1) |
| lightning (68★) | Go | with fix | very slow (static) | — | skiplist multi-level predecessor fix — no lost resting orders |
| ridulfo (69★) | Python | with fix | very slow (static) | ~400k/s | consistent total order in __lt__ — no priority inversion / lost cancels [#10](https://github.com/ridulfo/order-matching-engine/issues/10) |
| khrapovs ‡ | Python | with fix | very slow (pure Python) | — | MIT; orders_by_expiration not pruned on fill [#25](https://github.com/khrapovs/OrderBookMatchingEngine/issues/25); author: a senior ML engineer at ING (bank) |
| dabrowdev | TypeScript | as shipped | very slow (static) | — | MIT; O(n²) cancel/sweep, OrderQueue.remove reindexes level; Codeberg — filing TODO |
| ms_engine | TypeScript | with fix | very slow | — | don't rest a no-liquidity market order's remainder at sentinel −1 (market-order path only, off the limit-only workload) |
| lsamber | Java | with fix | very slow | — | stable same-price FIFO ordering so a LocalDateTime.now() collision can't reorder the PriorityQueue [#1](https://github.com/LS-Amber/financial-trading-system/issues/1) |
| m15102785298 | Java | as shipped | very slow | — | binary-search LinkedList via get(mid) → O(n² log n) deep-book build [#1](https://github.com/15102785298/Matching-algorithm/issues/1) |

*very slow* = the engine **completes** its weakest scenario but at worst-case throughput below ~0.01 M/s (distinct from *infeasible*, which cannot finish the scenario within the budget).

Conformance is a correctness property, not a quality ranking; these engines span every common book architecture (flat arrays, hierarchical bitmaps, RB / AVL / B+ trees, skip lists, intrusive FIFO queues) across 16 languages. `CORRECTNESS_FINDINGS.md` carries a one-line finding and the filed-issue link for each (the issue documents the patch).
