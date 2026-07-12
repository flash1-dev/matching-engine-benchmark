# Industry-authored engines

A cross-cutting view of the unified roster: the **52** engines written by people who publicly
claim a professional trading-industry role — engineers and quants at market-making, prop-trading,
and hedge-fund firms, exchange and vendor staff. It also includes the official vendor/org engines
and the production DEX matchers. Every engine here also appears in
[`CONSENSUS_CONFORMING_ENGINES.md`](CONSENSUS_CONFORMING_ENGINES.md) or
[`NON_CONFORMING_ENGINES.md`](NON_CONFORMING_ENGINES.md) (and in the
[`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) roster); this file only gathers them in one
place.

Of the 52: **15 conform as shipped** (two of them measured accessor-only), **18 conform only after
a documented fix**, and **19 diverge, cannot finish their worst scenario, or crash**. The fastest
worst-case among the 52 is **6.825 M/s**; the FlashOne reference measures **33.20 M/s** on the same
workload. The per-row findings below carry the specifics; the filed issue for each is linked in
[`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md). Each row is a snapshot at that engine's
pinned commit — several have since been fixed upstream — and most are personal side projects that
reflect their authors' own goals. No number or finding here ranks or judges an author's engineering quality — the harness reports what each pinned commit does. Affiliations are as the authors publicly state them, not independently verified.

**‡** = authored by a professional trading-industry engineer — a **personal side project with no
commercial intent, not their employer's work**, except where the Author column explicitly labels an
official vendor/org repo. Affiliations are as the authors publicly state them (GitHub profile /
bio), not independently verified by us. · **★** = repository has 50+ GitHub stars. Rows are sorted
by worst-case throughput — the lowest of an engine's five scenario results, the benchmark's
definitional result.

> **On authorship and affiliations.** Each ‡ engine is an individual developer's **personal side
> project with no commercial intent** — not their employer's product or work. **No engine here is a
> company's official work unless its author has explicitly published it as such**; the few official
> vendor / protocol / org repositories are the ones labeled as such in the Author column. The
> professional affiliations shown are the **authors' own public self-descriptions** (GitHub profile,
> bio, or a linked résumé), reproduced as stated and **not independently verified by us** as facts of
> employment — offered as context on the community that builds matching engines, not as a statement
> about, or on behalf of, any named company.

## Official / production repos

Unlike the personal side projects that make up most of this file, these **9** are official
repositories — a vendor's shipped product, an exchange's own engine, or a firm's org repo. Of the
9: **3 conform as shipped**, **2 conform after a documented fix** (among them the 10,236★
StockSharp, whose same-price orders could match out of arrival order at the pinned commit), and **4 diverge or cannot
finish** (among them the Lykke exchange's engine, whose static deep-book run exceeded the 2 M-message budget at the pinned commit).

| Engine | Lang | Author (as publicly claimed) | Conformance | Worst-case M/s | Finding (one line) |
|:--|:--|:--|:--|--:|:--|
| parity (502★) ‡ | Java | paritytrading — open-source org repo (self-described 'open source trading technologies') | as shipped | 2.21 | RB-tree: TreeSet + fastutil id-map |
| coralme (56★) ‡ | Java | CoralBlocks — official vendor repo | as shipped | 1.97 (flash-crash) | clean |
| stocksharp (10236★) ‡ | C# | StockSharp — official vendor repo | with fix | 1.64 (swing-25) | same-price orders could match out of arrival order (Dictionary enumeration replaces FIFO); conforms with the documented one-line fix; [#681](https://github.com/StockSharp/StockSharp/issues/681) |
| trademacher ‡ | Java/JNI | TradeMatcher — the org's own published matching-engine project | as shipped | 0.15 (swing-25) | clean |
| zorrofix ‡ | C++ | DSEC Capital's own repo (org bio: 'Algo Trading Tech Shop') | with fix | 0.01 (static) | sweep loop never checks whether the aggressor has closed → execute(0)→NAN→abort [#12](https://github.com/dsec-capital/zorro-fix/issues/12) |
| abides ‡ | Python | JPMorgan research org repo (author: Tucker Balch) | non-conforming | infeasible (static) | byte-identical on small cells, but static times out at 2 M |
| lykke ‡ | Kotlin | the Lykke exchange — official repo | non-conforming | infeasible (static) | fast on normal; its static deep-book run exceeded the 2 M-message budget at the pinned commit (conforms on the scenarios it completes) |
| opexdev ‡ | Kotlin | OPEX — official org repo (open-source crypto-exchange platform) | non-conforming | infeasible (2 M) | modify after a partial fill rests less than requested (stale filledQuantity) + an unconditional O(book-depth) tax per message → infeasible at 2 M; [Bug Report] [#688](https://github.com/opexdev/core/issues/688) |
| buttercoin ‡ | CoffeeScript | Buttercoin — a Bitcoin exchange that closed in 2015; open-sourced engine | non-conforming | crash | partial residual re-inserted under inverted price key → book locks; buttercoin/buttercoin-engine, MIT; [#9](https://github.com/buttercoin/buttercoin-engine/issues/9) |

## Production DEX matchers

The order books that run (or ran) real decentralized exchanges — a different deployment target for
the same matching problem. Measured de-chained as native code, **5 conform** (Serum, Manifest,
Phoenix cleanly; dYdX and Vega accessor-only) at 0.08–2.6 M/s worst-case. The **4 measured through
their chain's VM cannot complete the 2 M-message tape under VM interpretation** — an
execution-environment cost plus documented per-call design caps (gas-safety parameters, not
correctness defects): under revm interpretation Clober runs at ~0.02 M/s and its 2^15
orders-per-tick cap (a representational limit) diverges the deep book; DeepBook's own MAX_FILLS=100
per-call cap (a documented gas-safety cap, not a defect) truncates a deep sweep; Econia and Osmosis
exceed the 600 s watchdog under the Move VM and wasmer (~444 ms/message).

| Engine | Lang | Author (as publicly claimed) | Conformance | Worst-case M/s | Finding (one line) |
|:--|:--|:--|:--|--:|:--|
| serum ‡ | Rust | Project Serum — the original Solana on-chain order book | as shipped | 2.625 (static) | de-chained Solana CLOB — clean |
| manifest ‡ | Rust | Manifest (production Solana CLOB; pinned via the Bonasa-Tech repo) | as shipped | 2.145 (static) | de-chained — clean |
| phoenix ‡ | Rust | Ellipsis Labs (founders ex-Jane Street/Citadel) — official repo | as shipped | 1.5 (static) | de-chained production Solana CLOB — clean |
| dydx ‡ | Go | Antonio Juliano (dYdX founder) | as shipped | 0.11 (swing-25) | de-chained v4 memclob (accessor-only) |
| vega ‡ | Go | Barney Mannerings (designed a London Stock Exchange matcher; Vega Protocol) | as shipped | 0.08 (static) | accessor-only |
| clober ‡ | Solidity/EVM | Clober — official org repo (v2-core, production EVM CLOB) | non-conforming | 0.02 (static diverges) | de-chained via revm; the static deep-book scenario diverges at Clober v2's own 32,768-order-per-tick (2^15) OrderId cap — conforms 400/400 on the four feasible scenarios; a representational limit (QuantCup-class), not a correctness defect; ~0.02 M/s under revm interpretation |
| econia ‡ | Aptos Move | Econia Labs — official org repo (production Aptos CLOB) | non-conforming | infeasible (2 M) | de-chained via the Aptos Move VM (FakeExecutor); a deep same-price sweep exceeds the 600 s watchdog (~444 ms/msg, gate 32/33) → infeasible |
| osmosis ‡ | CosmWasm/Rust | Osmosis — official org repo (sumtree-orderbook, production Cosmos CLOB) | non-conforming | infeasible (2 M) | infeasible at 2 M (the wasmer-driven audit exceeds the 600 s watchdog); separately, a latent stale-best-tick pointer (`next_bid_tick`/`next_ask_tick` never retracted on a cancel or exact-drain sweep) fails the gate's state dimension without the fix; stale-best-tick bug filed [#211](https://github.com/osmosis-labs/orderbook/issues/211) |
| deepbook ‡ | Sui Move | Mysten Labs — official org repo (DeepBook v3, Sui's CLOB) | non-conforming | infeasible (2 M) | de-chained via the Sui Move VM (simulacrum); a deep recursive sweep is truncated by DeepBook's own MAX_FILLS=100 per-call cap — a documented gas-safety design cap, not a defect (gate 32/33) + infeasible at 2 M |

## The full industry-authored roster

FlashOne — the harness publisher's production engine — is shown at the top as the reference point;
it is not counted in the 52.

| Engine | Lang | Author (as publicly claimed) | Conformance | Worst-case M/s | Finding (one line) |
|:--|:--|:--|:--|--:|:--|
| FlashOne | C++ | Flash One Technologies LLC | as shipped | 33.20 (normal) | reference target |
| ndfex ‡ | C++ | Matthew Belcher (ex-Citadel Securities, 17y HFT) | as shipped | 6.825 (swing-25) | std::map RB-tree book (clean) |
| yashkukrecha ‡ | C++ | incoming at Jump Trading | as shipped | 6.26 (normal) | two priority_queues + timestamp FIFO tiebreak (clean; fastest pro-wave conformer) |
| daniele ‡ | C++ | an Optiver engineer | with fix | 3.60 (static) | fill-report reads a maker freed in the same fill (matching is correct); [#1](https://github.com/Daniele122898/Trading-Engine/issues/1) |
| serum ‡ | Rust | Project Serum — the original Solana on-chain order book | as shipped | 2.625 (static) | de-chained Solana CLOB — clean |
| parity (502★) ‡ | Java | paritytrading — open-source org repo (self-described 'open source trading technologies') | as shipped | 2.21 | RB-tree: TreeSet + fastutil id-map |
| shivaganapathy ‡ | C++ | an IMC engineer | as shipped | 2.15 (normal) | two priority_queues + timestamp FIFO tiebreak (clean) |
| manifest ‡ | Rust | Manifest (production Solana CLOB; pinned via the Bonasa-Tech repo) | as shipped | 2.145 (static) | de-chained — clean |
| coralme (56★) ‡ | Java | CoralBlocks — official vendor repo | as shipped | 1.97 (flash-crash) | clean |
| robdev ‡ | Rust | a CME Group engineer | with fix | 1.76 (static) | clear the emptied price level on cancel, return the real cancel result, and kill the IOC residual — all latent as-shipped (match path immune; the stale best_price is caught by the gate's state audit) [#1](https://github.com/rob-DEV/match-engine/issues/1) |
| stocksharp (10236★) ‡ | C# | StockSharp — official vendor repo | with fix | 1.64 (swing-25) | same-price orders could match out of arrival order (Dictionary enumeration replaces FIFO); conforms with the documented one-line fix; [#681](https://github.com/StockSharp/StockSharp/issues/681) |
| matchina ‡ | Rust | at GSR (crypto market maker) | with fix | 1.60 (static) | taker-exhaustion guard — no phantom zero-quantity trades; fixed upstream — `RESOLVED_FINDINGS.md` [#3](https://github.com/fran0x/matchina/issues/3) |
| phoenix ‡ | Rust | Ellipsis Labs (founders ex-Jane Street/Citadel) — official repo | as shipped | 1.5 (static) | de-chained production Solana CLOB — clean |
| tembolo ‡ | C | a quantitative developer at Tradeweb | with fix | 1.475 (swing-25) | two capacity ceilings (8192-order pool silent-drop + 512 price-level abort) [#1](https://github.com/tembolo1284/matching-engine-c/issues/1) |
| koral ‡ | C++ | a Coinbase software-engineering intern | as shipped | 1.255 (normal) | FIX exchange (clean; thread-affinity plumbing only) |
| ironcrypto ‡ | Rust | self-described 'TradFi/DeFi Quant' | with fix | 1.04 (6.2 normal) | no-license; adapter restores engine's removed cancel impl (faithfulness caveat) |
| trusted ‡ | Rust | a KRX market-maker at IBK Securities | with fix | 0.925 (static) | latent bid-side market-order double-subtract underflow [#9](https://github.com/JunbeomL22/trusted/issues/9) |
| kennethzhang ‡ | C++ | a Squarepoint quant researcher | with fix | 0.86 (static) | price the limit-vs-limit cross at the resting maker, not the taker (the adapter normalizes it today) [#1](https://github.com/kennethZhangML/TradingClientExchange/issues/1) |
| javalob ‡ | Java | Ash Booth (JPMorgan) | as shipped | 0.86 (swing-40) | teaching LOB (clean) |
| swirly ‡ | Java | a trading-systems developer; co-founder of Reactive Markets | as shipped | 0.79 (swing-40) | clean — native revise changes only lots, so modify = cancel+reinsert per contract |
| damian ‡ | Kotlin | Damian Howard (20y at a bank) | as shipped | 0.38 (static) | clean |
| pyob ‡ | Python | an FX e-trading quant at mBank | with fix | 0.30 (swing-25) | deque IndexError on a full fill + stale best_price after cancel [#1](https://github.com/wegar-2/pyob/issues/1) |
| joaquinbejar ‡ | Rust | a quant developer at Capital Delta | with fix | 0.29 (static) | Book::replace crossing modify never re-matches → rests a crossed book [#59](https://github.com/joaquinbejar/hft-clob-core/issues/59) |
| oceanbook ‡ | Go | self-described HFT developer (bio 'HFT / C++ / Go'; @spectra-fund) | with fix | 0.26 (flash-crash) | Depth writes quantity into the qty field, not the price field [#44](https://github.com/draveness/oceanbook/issues/44) |
| rakuzen25 ‡ | C++ | an Optiver intern | with fix | 0.18 (flash-crash) | within-level FIFO fix (swap-with-last broke arrival order); uint16 ceiling residual; issues disabled upstream |
| sculd ‡ | Python | a quant/developer who has worked at Two Sigma | with fix | 0.18 (swing-25) | guard unknown-id cancel/status against KeyError and skip cancelled heads in _get_best_price — latent as-shipped (matching path unaffected) [#1](https://github.com/sculd/orderbook_practice_python/issues/1) |
| trademacher ‡ | Java/JNI | TradeMatcher — the org's own published matching-engine project | as shipped | 0.15 (swing-25) | clean |
| OrderBook-rs (477★) ‡ | Rust | a quant developer at Capital Delta | with fix | 0.13 (static) | partial-fill maker keeps FIFO priority (push_front, not re-queue to tail); fixed upstream — `RESOLVED_FINDINGS.md` [#88](https://github.com/joaquinbejar/OrderBook-rs/issues/88) |
| dydx ‡ | Go | Antonio Juliano (dYdX founder) | as shipped | 0.11 (swing-25) | de-chained v4 memclob (accessor-only) |
| qa-rs ‡ | Rust | a private-fund manager (Shanghai Binghao) | with fix | 0.09 (static) | OrderQueue lazy-deletion — same-id reinsert leaves a stale heap entry [#1](https://github.com/yutiansut/qa-rs/issues/1), `get_depth` over-counts a plain cancel until swept [#2](https://github.com/yutiansut/qa-rs/issues/2); + 5 latent `Orderbook` match-loop bugs off the limit-only workload: 1000-id recycle drops orders [#3](https://github.com/yutiansut/qa-rs/issues/3), market remainder rested not killed [#4](https://github.com/yutiansut/qa-rs/issues/4), amend skips the crossing check [#5](https://github.com/yutiansut/qa-rs/issues/5), NaN price passes validation [#6](https://github.com/yutiansut/qa-rs/issues/6), per-order sweep recursion overflows the stack [#7](https://github.com/yutiansut/qa-rs/issues/7) |
| vega ‡ | Go | Barney Mannerings (designed a London Stock Exchange matcher; Vega Protocol) | as shipped | 0.08 (static) | accessor-only |
| amer ‡ | Java | a former Nasdaq engineer | non-conforming | 0.02 (static) | same-price orders execute LIFO not FIFO — `>=`/`<=` insertion splices ahead of equal-priced peers (33/40 sweep cells diverge); unconditional O(depth) contra-scan → deep 2 M runs can exceed the watchdog; [#1](https://github.com/AmerSurkovic/MatchingEngine/issues/1) |
| clober ‡ | Solidity/EVM | Clober — official org repo (v2-core, production EVM CLOB) | non-conforming | 0.02 (static diverges) | de-chained via revm; the static deep-book scenario diverges at Clober v2's own 32,768-order-per-tick (2^15) OrderId cap — conforms 400/400 on the four feasible scenarios; a representational limit (QuantCup-class), not a correctness defect; ~0.02 M/s under revm interpretation |
| zorrofix ‡ | C++ | DSEC Capital's own repo (org bio: 'Algo Trading Tech Shop') | with fix | 0.01 (static) | sweep loop never checks whether the aggressor has closed → execute(0)→NAN→abort [#12](https://github.com/dsec-capital/zorro-fix/issues/12) |
| ghosh (677★) ‡ | C++ | a low-latency trading-systems developer and Packt author | with fix | very slow | MIT; flat 256-slot price index shared by both sides had no collision handling → cross-side bucket merge crash; + a 1,048,576 order-id cap; both fixed [#9](https://github.com/PacktPublishing/Building-Low-Latency-Applications-with-CPP/issues/9) |
| khrapovs ‡ | Python | a senior ML engineer at ING (bank) | with fix | very slow (pure Python) | MIT; orders_by_expiration not pruned on fill [#25](https://github.com/khrapovs/OrderBookMatchingEngine/issues/25) |
| abides ‡ | Python | JPMorgan research org repo (author: Tucker Balch) | non-conforming | infeasible (static) | byte-identical on small cells, but static times out at 2 M |
| bitex ‡ | Python | Rodrigo Souza (founder/CEO of BlinkTrade, the open-source platform behind the Foxbit exchange) | non-conforming | infeasible (static) | correct on normal/swings, but the static deep-book scenario times out at 2 M |
| figgie ‡ | OCaml | Ben Millwood (ex-Jane Street) — figgie is Jane Street's *Figgie* card game (a FIFO matcher), not a commercial engine | non-conforming | infeasible (static) | byte-identical where it completes, but static times out at 2 M |
| isaaccheng ‡ | Python | ex-T. Rowe Price fixed-income quant developer | non-conforming | infeasible (static) | byte-identical where it completes, but static times out at 2 M |
| ismailfer ‡ | Java | self-described systematic trading developer / quant trader | non-conforming | infeasible (2 M) | `processTrade()` clears the wrong side's `active` flag on a full fill — truncates multi-order sweeps and orphans resting quantity (a filled id can also never be reused); diverges + infeasible at 2 M; [Bug Report] [#1](https://github.com/ismailfer/exchange-simulator/issues/1) |
| liqian ‡ | C++ | ex-Virtu Financial quant trader | non-conforming | infeasible (2 M) | O(20 M-tick) domain scan per order → infeasible at 2 M; occupancy bitsets never skip empty levels; [#1](https://github.com/QuantTradingWithLi/high_perf_order_matching/issues/1) |
| lykke ‡ | Kotlin | the Lykke exchange — official repo | non-conforming | infeasible (static) | fast on normal; its static deep-book run exceeded the 2 M-message budget at the pinned commit (conforms on the scenarios it completes) |
| nilesh05apr ‡ | C++ | a Tower Research Capital SWE intern | non-conforming | infeasible (2 M) | matches only one counterparty per order; O(n²) re-sort → infeasible at 2 M; [#1](https://github.com/nilesh05apr/TradeSim/issues/1) |
| opexdev ‡ | Kotlin | OPEX — official org repo (open-source crypto-exchange platform) | non-conforming | infeasible (2 M) | modify after a partial fill rests less than requested (stale filledQuantity) + an unconditional O(book-depth) tax per message → infeasible at 2 M; [Bug Report] [#688](https://github.com/opexdev/core/issues/688) |
| pylob ‡ | Python | Ash Booth (JPMorgan) | non-conforming | infeasible (static) | SQLite-backed; static times out at 2 M; 2 bugs [#8](https://github.com/DrAshBooth/PyLOB/issues/8) |
| econia ‡ | Aptos Move | Econia Labs — official org repo (production Aptos CLOB) | non-conforming | infeasible (2 M) | de-chained via the Aptos Move VM (FakeExecutor); a deep same-price sweep exceeds the 600 s watchdog (~444 ms/msg, gate 32/33) → infeasible |
| osmosis ‡ | CosmWasm/Rust | Osmosis — official org repo (sumtree-orderbook, production Cosmos CLOB) | non-conforming | infeasible (2 M) | infeasible at 2 M (the wasmer-driven audit exceeds the 600 s watchdog); separately, a latent stale-best-tick pointer (`next_bid_tick`/`next_ask_tick` never retracted on a cancel or exact-drain sweep) fails the gate's state dimension without the fix; stale-best-tick bug filed [#211](https://github.com/osmosis-labs/orderbook/issues/211) |
| raunakchopra ‡ | C++ | a Flow Traders engineer | non-conforming | crash (sweep) | recursive re-submit overflows the stack; not price-time; O(trades²) I/O; [#1](https://github.com/raunakchopra/OrderBook/issues/1) |
| buttercoin ‡ | CoffeeScript | Buttercoin — a Bitcoin exchange that closed in 2015; open-sourced engine | non-conforming | crash | partial residual re-inserted under inverted price key → book locks; buttercoin/buttercoin-engine, MIT; [#9](https://github.com/buttercoin/buttercoin-engine/issues/9) |
| soham ‡ | C++ | an iRage quant-analyst intern | non-conforming | — | binary Yes/No prediction-market matcher — prices hard-validated to [1, 99]; rejects the benchmark's full-range tapes in full by design (not a FIFO defect); internally sound (author-run differential fuzzing; in-domain checks match exactly), no bug report was filed |
| lethalazo ‡ | C++ | at Marshall Wace (hedge fund) | non-conforming | — | add-only book, missing the cancel/modify operations the benchmark drives; 2 bugs [#1](https://github.com/lethalazo/cpp-order-matching-engine/issues/1) |
| deepbook ‡ | Sui Move | Mysten Labs — official org repo (DeepBook v3, Sui's CLOB) | non-conforming | infeasible (2 M) | de-chained via the Sui Move VM (simulacrum); a deep recursive sweep is truncated by DeepBook's own MAX_FILLS=100 per-call cap — a documented gas-safety design cap, not a defect (gate 32/33) + infeasible at 2 M |

As everywhere in this repository: these observations are a reproducible, time-stamped *snapshot* of
a specific pinned commit, offered back to the authors — several are already fixed upstream — and
**not a judgment of anyone's engineering quality**; personal side projects reflect their authors' goals,
which may differ from a venue's. The one-line finding and the filed-issue link per engine are in
[`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md).
