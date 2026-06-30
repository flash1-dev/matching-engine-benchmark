# Non-conforming engines

Companion to the [`README.md`](README.md) engine roster. A one-line finding per engine is in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) (full mechanism in the linked issue); the pre-run conformance gate is described in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

These **59** engines are FIFO matchers that, on the byte-identical consensus check, either **diverge** on the canonical workload (over-matching, lost or orphaned orders, wrong execution price, quantity non-conservation, crashes, deadlocks), are **infeasible at 2 M** (correct on every cell they complete but unable to finish their slowest scenario within the 2-million-message budget), or **crash** in their weakest scenario — **or** they conform on the canonical workload but carry a known latent defect the workload never triggers (caught by the pre-run conformance gate or by source review).

Non-conforming means only **"the output differs from the consensus, the run cannot complete, or a gate/source-review defect"** — it is not a judgment of engineering quality (`CORRECTNESS_FINDINGS.md` and the audit notes carry the mechanics).

| Engine | Language | How it diverges | Worst-case M/s | Published figure | Notes |
|:--|:--|:--|--:|:--|:--|
| mtengine | Rust | flat array + bitmap, bounded price domain — diverges on `flash-crash` / the widest swings | 6.82 (static) | — | 3-tier bitset + level array + FIFO |
| cheetah | Rust | cancel searches the opposite book; a 10k-id de-dup window drops non-monotonic ids | 5.25 (normal) | — | BTreeMap keyed (price, id); fixed upstream — `RESOLVED_FINDINGS.md` (pin stays pre-fix) |
| lanpishu (385★) | C++ | depth non-conservation + broken RB-tree delete-fixup (wrong best price; can hang) | 2.66 (static) | — | hand-rolled RB-tree + std::map levels |
| femto_go | Go | VALID on `static`/`normal`; diverges deterministically on the three wide-swing scenarios | 2.24 (normal) | >10 M/s | flat price array + slot index |
| makersu | Go | RB-tree iterator invalidated mid-sweep → multi-level sweeps skip levels (separate from the filed FIFO fix) | 1.80 (swing-25) | — | documented FIFO fix applied; this is a distinct defect [#1](https://github.com/makersu/go-exchange-matching/issues/1) |
| piquette | Go | bid/ask maps never populated → every cancel rejected; best-ask uses `>` not `<` | 1.28 (flash-crash) | — | AVL tree + DLL FIFO + hash |
| abyssbook | Zig | swapRemove scrambles FIFO + SIMD over-fill (no Zig toolchain to apply the filed fix) | 0.12 (static) | — | tick array + 4-tier bitmap [#41](https://github.com/aldrin-labs/abyssbook/issues/41) |
| gavincyi (358★) | Python | crashes on a cancel/modify of a fully-filled order (conformance gate) | 0.01 (static) | ~92 ns/op | dict + list FIFO (best by min/max scan) |
| jxxxq | OCaml | combine_orders collapses same-price orders → FIFO / identity lost | — | — | needs consume-path rework, not one-line patch [#1](https://github.com/Jxxxq/ocaml-orderbook-engine/issues/1) |
| oldfritter | Go | limit-vs-limit never matches; LimitTop reads wrong tree | — | — | fixed → match loop recurses forever; fill layer rework [#4](https://github.com/oldfritter/matching/issues/4) |
| luminengine | Rust | async matcher → non-deterministic output, can't be quiesced | — | — | architectural; AGPL-3.0, no bug filed |
| realyarilabs | Elixir | expired maker still trades; cancel guard inverted | — | — | realyarilabs/exchange; [#134](https://github.com/realyarilabs/exchange/issues/134) |
| bahbah94 | Haskell | computes trades but never removes filled liquidity | — | — | bahbah94/Order-Book-Haskell; [#1](https://github.com/bahbah94/Order-Book-Haskell/issues/1) |
| zhaocong6 | Go | cancel drops every order at the price level, not just target | — | — | zhaocong6/match; [#1](https://github.com/zhaocong6/match/issues/1) |
| pyobsim | Python | remove deletes only head; match mutates level while iterating | — | — | jmcph4/PyOBSim; [#2](https://github.com/jmcph4/PyOBSim/issues/2) |
| lua_matcher | Lua | same-price FIFO insert violation: later order queued ahead | — | — | geek-sajjad/crypto-matching-engine-lua, no-license; [#1](https://github.com/geek-sajjad/crypto-matching-engine-lua/issues/1) |
| knocte_fx | F# | marketable limit crosses then rests → crossed book + lost fill | — | — | gitlab.com/knocte/FX, MIT; GitLab — filing TODO |
| rinok | Clojure | buy crossing lower sell prints at buy's price (buy-initiated) | — | — | film42/rinok, EPL-1.0; [#2](https://github.com/film42/rinok/issues/2) (resolved) |
| opencx | Go | multi-level fill drops residual; uint64 size underflows | — | — | mit-dci/opencx, MIT; held (no draft) |
| glinscott | JavaScript | cancelling last order strands empty Limit → null-deref | — | — | glinscott/jsorderbook, no-license; [#2](https://github.com/glinscott/JSOrderbook/issues/2) |
| aas2015001 | Java | exact fill leaves zombie 0-qty; one order per level | — | — | aas2015001/ordermatchingengine, no-license; [#1](https://github.com/aas2015001/OrderMatchingEngine/issues/1) |
| buttercoin | CoffeeScript | partial residual re-inserted under inverted price key → book locks | — | — | buttercoin/buttercoin-engine, MIT; [#9](https://github.com/buttercoin/buttercoin-engine/issues/9) |
| hinokamikagura | Java | updateOrderQuantity loses the order's FIFO position | — | — | hinokamikagura/crypto-wallet-engine, no-license; [#1](https://github.com/hinokamikagura/crypto-wallet-engine/issues/1) |
| hyobyun | JavaScript | heap comparator reads .price off Node not .key → NaN | — | — | hyobyun/exchangeengine, MIT; [#3](https://github.com/hyobyun/exchangeengine/issues/3) |
| devashishpuri | TypeScript | inner loop uses getObjMin not getObjMax → bids swept out of order | — | — | sell sweeping ≥3 bid levels jumps to cheapest remaining bid [#1](https://github.com/devashishpuri/ExchangeMatchingEngine/issues/1) |
| hillside6 | Java | best bid taken as get(size-1) → sell fills newest-first (LIFO) | — | — | bid-side LIFO not FIFO among same-price ties [#1](https://github.com/hillside6/matching/issues/1) |
| jiker_burce | Rust | match_price (Buy,Sell) arm uses aggressor's price not maker's | — | — | buy sweep prints every fill at buy's price [#1](https://github.com/jiker-burce/matching-engine/issues/1) |
| jlome | Java | Collections.min picks lowest bid for sell → inverted sell-side priority | — | — | marketable sell can rest, leaving a crossed book [#1](https://github.com/Alessandro-Salerno/JLOME/issues/1) |
| mmrath | Rust | cancel leaves price-level entry → reused slot aliases live order | — | — | phantom level → spurious cross at wrong price [#1](https://github.com/mmrath/oms/issues/1) |
| lyqingye | Java | intrinsic notional-budget order design, reproduced faithfully — not a defect | — | — | separate O(n²) non-marketable-scan perf finding unfileable (repo archived), held |
| amansardana | Go | fill loop lacks amount>0 guard → over-sweeps, unlinks resting order | — | — | spurious zero-amount fill; order behind it unlinked [#1](https://github.com/amansardana/matching-engine/issues/1) |
| jenyayel | C# | same-price order inserted at arbitrary BinarySearch index → lost time priority | — | — | held — repository has issues disabled (unfileable) |
| pyrsquant | Rust | remove_order uses swap_remove → non-tail cancel breaks FIFO | — | — | latent on canonical seed, surfaced by the gate [#1](https://github.com/tombelieber/py-rs-quant/issues/1) |
| harshsuiiii | TypeScript | pops from tail of best-first side → matches worse price, deletes bystander | — | — | fillOrders scans each side from the tail [#1](https://github.com/harshsuiiii/LOW-LATENCY-TRADING-MATCHING-ENGINE-ORDERBOOK-/issues/1) |
| laymats | Java | returns earliest crossable ask not lowest → prints above best offer | — | — | buy matches oldest/dearest ask [#19](https://github.com/laymats/auto.trade.engie/issues/19) |
| nirvanasu | Go | unstable sort.Slice price-only comparator loses time priority above 12 orders | — | — | 14-case gate passes; canonical workload diverges [#1](https://github.com/nirvanasu00-cpu/Go-Exchange-Core/issues/1) |
| murtyjones | TypeScript | diverges by pricing convention only | — | — | documented buyer-best-price convention; book state consensus-correct; no bug filed |
| nexbook | Scala | diverges by pricing convention only | — | — | deliberate midpoint deal-price convention; matching/FIFO/quantities consensus-correct; no bug filed |
| afterworkguinness | Java | builds a whole-book String per op → O(book)/op; correct where it completes, infeasible at 2 M | infeasible (2 M) | — | builds whole-book String per insert/match → O(book)/op; conforms where completes [#2](https://github.com/afterworkguinness/matching-engine/issues/2) |
| apexmatch | Go | won't build at HEAD (six files miss their package clause); infeasible at 2 M once fixed | infeasible (2 M) | — | won't build — six .go files lack package clause [#1](https://github.com/luka2049/apexmatch/issues/1) |
| chessbr | Rust | correct with the best-bid fix but O(n²) (Vec insert/remove + O(book) cancel rebuild) → infeasible at 2 M | infeasible (2 M) | — | matching correct, throughput not [#4](https://github.com/chessbr/rust-exchange/issues/4) |
| coinexchange | Java | correct but exceeds the 2 M watchdog on its slowest scenario | infeasible (2 M) | — | byte-identical on completed cells; claimed stale-modify-ack did not reproduce |
| iwtxokhtd83 | Go | non-terminating on the first multi-fill without a 1-line fix; infeasible at 2 M even with it | infeasible (2 M) | — | consumed head removed after loop → non-terminating; one-line termination fix [#12](https://github.com/iwtxokhtd83/MatchEngine/issues/12) |
| jogeshwar | Rust | correct but exceeds the 2 M watchdog on its slowest scenario | infeasible (2 M) | — | byte-identical with documented fill-size fix [#1](https://github.com/jogeshwar01/exchange/issues/1) |
| jugutier | Java | static scenario infeasible at 2 M; latent update() null-deref on an empty side | infeasible (static) | — | PriorityOrderBook.update() null-derefs when order's side has no resting queue [#1](https://github.com/jugutier/OrderBook/issues/1) |
| liqian | C++ | O(20 M-tick) domain scan per order → infeasible at 2 M | infeasible (2 M) | — | occupancy bitsets never skip empty levels; [#1](https://github.com/QuantTradingWithLi/high_perf_order_matching/issues/1) |
| lmxdawn | Java | correct but exceeds the 2 M watchdog on its slowest scenario | infeasible (2 M) | — | byte-identical with phantom-fill fix [#11](https://github.com/lmxdawn/exchange/issues/11) |
| masroor47 | Python | O(n²) trade-history rebuild (pd.concat per add); static scenario infeasible at 2 M | infeasible (static) | — | held/no-license; add_order pd.concat rebuild every call → O(n²) [#2](https://github.com/masroor47/limit-order-book/issues/2) |
| nilesh05apr | C++ | matches only one counterparty per order; O(n²) re-sort → infeasible at 2 M | infeasible (2 M) | — | [#1](https://github.com/nilesh05apr/TradeSim/issues/1) |
| peatio | Ruby | correct but exceeds the 2 M watchdog on its slowest scenario | infeasible (2 M) | — | Ruby rbtree engine; byte-identical on completed cells |
| vinci217 | Go | GetMarketDepth returns arbitrary, unsorted levels (ranges a Go map); infeasible at 2 M | infeasible (2 M) | — | GetMarketDepth returns arbitrary, unsorted price levels (ranges a Go map) [#2](https://github.com/Vinci-217/trading-system/issues/2) |
| zzsun777 | C++ | correct but exceeds the 2 M watchdog on its slowest scenario | infeasible (2 M) | — | O(n) by-owner find; byte-identical on completed cells |
| big_order_book | JavaScript | filled maker never removed from orderItemMap → null-list crash | crash | — | GPL: glue shipped, engine fetched at build [#1](https://github.com/Capitalisk/big-order-book/issues/1) |
| dylanlott | Go | derefs buyOrders[0] before empty check → sell into empty book panics | crash (empty book) | — | underlying: descending sell-side sort + wrong-side fill qty, left intact [#10](https://github.com/dylanlott/orderbook/issues/10) |
| hnodomar | C++ | Level& freed mid-sweep by book.erase, used through dangling ref | crash | — | heap use-after-free corrupts later matches at that price [#1](https://github.com/Hnodomar/Spot-Exchange/issues/1) |
| laffini | Java | crashes all five — `\|\|` should be `&&`, plus missing price-cross/empty-list guards | crash (all 5) | — | two sorted ArrayLists, re-sorted per insert |
| raunakchopra | C++ | recursive re-submit overflows the stack; not price-time; O(trades²) I/O | crash (sweep) | — | [#1](https://github.com/raunakchopra/OrderBook/issues/1) |
| wailo | C++ | crashes on static / diverges; the documented operator> fix does not restore time priority | crash (static) | — | binary-heap book, no same-price FIFO [#1](https://github.com/wailo/orderbook-matching-engine/issues/1) |
| zackienzle | C++ | hierarchical 4-tier bitmap, bounded price domain — crashes (OOB) on `swing-40`/`flash-crash` | crash (wide swings) | — | tick array + 4-tier bitmap + FIFO |

In the *Worst-case M/s* column, **crash** marks an engine that aborts in its weakest scenario (on seed 23; a note flags an engine that can hang); **infeasible (2 M)** marks an engine that is correct where it completes but cannot finish its slowest scenario within the 2-million-message budget; a number is the lowest of the engine's five scenario throughputs.

These findings are offered back, not aimed at anyone — each is a reproducible, time-stamped *snapshot* of a specific commit, not a verdict on a project's quality, and several integrated engines ship with a fix the reference adapter applies (`CORRECTNESS_FINDINGS.md` carries the one-line finding and filed-issue link for each).
