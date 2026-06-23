# Consensus-conforming engines

Companion to [`README.md`](README.md) (which shows the top 10 by worst-case throughput). A one-line finding and the filed-issue link per engine are in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) (the linked issue carries the full mechanism and patch); the pre-run conformance gate is in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

These **55** high-confidence engines (for 33 of them, with our suggested fix) reach byte-for-byte identical consensus on both the report stream and the book state across 100 random workload seeds (**+1 billion order messages** on each engine), and each also passes the pre-run conformance gate. **as shipped** = conforms unmodified; **with fix** = conforms after the minimal documented engine patch named (the filed upstream issue; one-line finding in `CORRECTNESS_FINDINGS.md`, full mechanics in the issue).

The **Worst-case M/s** column is each engine's lowest throughput across the five scenarios (weakest regime, seed 23, Graviton4 / Neoverse-V2, `-O3 -march=native`). 

| Engine | Language | Conformance | Worst-case M/s | Published figure | Notes |
|:-------|:---------|:------------|:---------------|:-----------------|:------|
| FlashOne      | C++      | as shipped        | 33.20 (normal) | — | reference target |
| geseq/cpp-orderbook | C++      | as shipped       | 7.94 (swing-25) | — | author-contributed C++ port of geseq/orderbook |
| CppTrader (1041★) | C++      | as shipped       | 7.26 (normal) | ~7.2M upd/s | a `ModifyOrder` defect off the canonical path is fixed upstream — `RESOLVED_FINDINGS.md` |
| Kautenja (309★) | C++ | with fix | 6.88 (normal) | — | reject a duplicate live order-id (no self-linked FIFO / UAF) |
| asthamishra | Rust | with fix | 5.60 (flash-crash) | — | bounds-check the tick array — no dropped orders above the ceiling |
| llc993 (154★) | Rust     | as shipped       | 5.43 (swing-40) | ~7.2M/s | BTreeMap + slab pool + intrusive time-queue (exchange-core-inspired) |
| hroptatyr/clob | C | as shipped       | 4.73 (normal) | ~6M/s | b+tree CLOB, `_Decimal64` (no patch) |
| mercury       | C++      | as shipped       | 3.94 (normal) | 3.2M/s | abseil b-tree |
| microexchange (62★) | C++      | as shipped       | 3.62 (flash-crash) | 2.24M/s | array + bitmap |
| Tzadiko (307★) | C++      | with fix | 3.39 (flash-crash) | — | IOC self-deadlock; two-site lock-wrapper fix |
| piyush (148★) | C++ | with fix | 3.28 (flash-crash) | ~160 M/s | cached best-ask self-heal — re-seat to the next set level, not only on an empty side |
| fmstephe (474★) | Go | with fix | 2.48 (static) | — | crossing trades print at the maker price, not the midpoint |
| coralme (56★) | Java     | as shipped       | 1.97 (flash-crash) | — | |
| robaho | C++ | with fix | 1.90 (swing-25) | 10–22 M/s | execute at the resting (maker) price, not the aggressor's limit |
| geseq         | Go       | as shipped       | 1.81 (swing-25) | 12.5–21M/s | a multi-level cross-through is fixed upstream — `RESOLVED_FINDINGS.md` |
| gocronx (84★) | Rust     | as shipped       | 1.77 (static) | ~17M/s | |
| apex | Rust | with fix | 1.62 (static) | — | execute at the maker price, not the aggressor's limit |
| matchina | Rust | with fix | 1.60 (static) | — | taker-exhaustion guard — no phantom zero-quantity trades |
| Exchange-core (2556★) | Java/JVM | as shipped        | 1.40 (flash-crash) | — | baseline; direct-access book, JNI per message |
| jiang         | Java     | with fix | 1.30 (swing-25) | — | 1-line `idMaps.remove(id)` so modify doesn't drop the order |
| limitbook | Rust | with fix | 1.16 (static) | ~30 M/s | partial-fill write-back — decrement the resting maker |
| m5487         | Go       | as shipped       | 1.15 (swing-25) | ~2.6M/s | skiplist + disruptor |
| i25959341 (550★) | Go | with fix | 0.72 (swing-25) | >300k/s | per-side Volume() correct after a partial fill |
| jlob          | Java/JNI | as shipped       | 0.71 (static) | ~127 ns/op | L3 RB-tree, working JNI adapter |
| mh2rashi | C++ | with fix | 0.70 (swing-40) | ~23k/s | 1-line `deleteOrder` list-corruption/crash fix |
| danielgatis | Go | with fix | 0.58 (swing-25) | — | normalize the decimal price key — equal prices share one level |
| QuantCup (211★) | C++      | as shipped        | 0.57 (flash-crash) | — | baseline; flat price-indexed array |
| gotrader (513★) | Go | with fix | 0.56 (swing-25) | 400k quote/s (net) | reject a modify of a fully-filled order (don't swallow-ack) |
| pyme (133★)   | Python   | as shipped       | 0.31 (swing-25) | ~150k/s | doubly-linked price levels (CPython embed) |
| dsirotkin | C++ | with fix | 0.31 (static) | — | cancel removes only the order, not the rest of the price level |
| oceanbook | Go | with fix | 0.26 (flash-crash) | — | Depth writes quantity into the qty field, not the price field |
| brprojects (186★) | C++  | as shipped       | 0.23 (swing-25) | ~1.4M/s | uncached-height AVL (perf-only) |
| dyn4mik3 (409★) | Python | with fix | 0.23 (swing-25) | — | 1-line `get_price` → `get_price_list` crash fix |
| trademacher   | Java/JNI | as shipped       | 0.15 (swing-25) | ~5M/s | |
| OrderBook-rs (477★) | Rust | with fix | 0.13 (static) | latency-focused | partial-fill maker keeps FIFO priority (push_front, not re-queue to tail) |
| dgtony (453★) | Rust | with fix | 0.09 (static) | — | widen the id-gen range + amend can't leave the book crossed |
| matchingo | Go | with fix | 0.08 (static) | — | UpdateVolume subtracts the consumed qty, not the remainder (depth audit) |
| pantelwar (75★) | Go | with fix | 0.07 (static) | — | remove hot-path debug logging + fix the MarshalJSON sell-side bug |
| jeog | C++      | as shipped       | 0.05 (flash-crash) | — | flat directly-indexed price vector |
| jxm35 | C++ | with fix | 0.05 (static) | 14 M/s | drop the redundant hand-splice (restores level accounting) + emit trade reports |
| Liquibook (1479★) | C++      | as shipped | 0.03 (static) | — | baseline; price-keyed multimap of lists (native IOC residual handled adapter-side, #43) |
| philipgreat (98★) | Rust     | with fix | 0.03 (static) | ~8 ns/order | 3 cancel/modify-path correctness fixes |
| cointossx (122★) | Java/JNI | with fix | 0.03 (static) | — | 2 fixes: B+Tree destructive `firstKey` + `AddOrderPreProcessor` wrong-side compare |
| auralshin     | Rust     | as shipped       | 0.01 (static) | — | |
| cspooner | Rust | with fix | very slow (static) | — | conserve quantity on a partial fill (+ 4 related fixes) |
| pgellert (65★) | Rust | with fix | very slow (swing-40) | — | don't drop the popped order + correct the stale price-bounds |
| mansoor (64★) | C++ | with fix | very slow (normal) | >20 M/s | bounds-check the price array — no OOB on wide swings |
| luo4neck | C++ | with fix | very slow (static) | — | match best-price-first (price-time priority) |
| pyxchange | C++ | with fix | very slow (static) | ~100k/s | monotonic (price,seq) key — no dropped same-tick same-price orders |
| darkpool | C++ | with fix | very slow (static) | — | report the execution at the maker price |
| fasenderos (202★) | TypeScript | as shipped  | very slow (static) | >300k/s | libnode embed |
| lobster (172★) | Rust    | as shipped       | very slow (swing-40) | — | BTreeMap + arena + id-index |
| techieboy (58★) | Rust   | with fix | very slow (swing-25) | ~10 µs/match | 2 fixes: spurious zero-qty fills + stale best-bid/ask |
| lightning (68★) | Go | with fix | very slow (static) | — | skiplist multi-level predecessor fix — no lost resting orders |
| ridulfo (69★) | Python | with fix | very slow (static) | ~400k/s | consistent total order in __lt__ — no priority inversion / lost cancels |

*very slow* = the engine did not clear the workload within the time budget in its weakest scenario (worst-case throughput below ~0.01 M/s).

Conformance is a correctness property, not a quality ranking; these engines span every common book architecture. `CORRECTNESS_FINDINGS.md` carries a one-line finding and the filed-issue link for each (the issue documents the patch).
