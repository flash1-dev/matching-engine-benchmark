# Matching Engine Algorithm Performance Challenge

A reproducible benchmark for limit-order-book matching engine algorithms: one deterministic
workload, one byte-identical correctness oracle, identical work for every engine. Published
with [*"The World's Fastest Matching Engine Algorithm"*](https://arxiv.org/abs/2606.01183)
(Flash One Technologies, 2026).[^paper]

The survey aims to cover every publicly available FIFO matching implementation without
duplicates; to request coverage of an engine, contact contact@flash1.com.

## Results

The harness has been run against **246 distinct matching engines** — every common FIFO book
architecture, 20+ source languages. **160 reproduce the byte-identical consensus** (110 of
them after a documented fix) and are listed in
[`CONSENSUS_CONFORMING_ENGINES.md`](CONSENSUS_CONFORMING_ENGINES.md); the other **86**
diverge, cannot finish their slowest scenario within the message budget, or crash
([`NON_CONFORMING_ENGINES.md`](NON_CONFORMING_ENGINES.md)).

The consensus oracle is also a bug-finder: the sweep surfaced **more than 267 distinct
correctness bugs across 199 of the 246 engines**, and **over 172 GitHub issues have been
filed upstream** — several already fixed by their maintainers
([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)). The full decomposition, per-engine verdicts,
and one-line findings are in [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md); the
industry-authored subset is broken out in
[`INDUSTRY_AUTHORED_ENGINES.md`](INDUSTRY_AUTHORED_ENGINES.md).

The **top 10 by worst-case throughput on seed 23** — each engine's lowest of the five
scenarios (seed 23, Graviton4 / Neoverse-V2, `-O3 -march=native`; median of 10 trials).
FlashOne is the harness publisher's algorithm, shown as a reference; it stands as a target to
beat on fully public, audited work.

**‡** = authored by a professional trading-industry engineer — a **personal side project, not an official repository of their employer**, except where the Notes explicitly label an official vendor/org repo. Affiliations are as the authors publicly state them, not independently verified by us. · **★** = repository has 50+ GitHub stars. The industry-authored subset is broken out in [`INDUSTRY_AUTHORED_ENGINES.md`](INDUSTRY_AUTHORED_ENGINES.md).

| Engine | Language | Conformance | Worst-case M/s | Published figure | Notes |
|:-------|:---------|:------------|:---------------|:-----------------|:------|
| FlashOne | C++ | as shipped | 33.20 (normal) | — | reference target |
| e820 / weekend-orderbook | C | with fix | 8.19 | — | singly-linked orphan + aggressor-price fix [#1](https://github.com/oldfifteenpoundy/weekend-orderbook/issues/1) |
| geseq/cpp-orderbook | C++ | as shipped | 7.94 (swing-25) | — | author-contributed C++ port of geseq/orderbook |
| melin | Rust | with fix | 7.86 | — | BSL-1.1; stop-trigger cascade single-pass [#2](https://github.com/melin-engine/melin/issues/2) |
| CppTrader (1041★) | C++ | as shipped | 7.26 (normal) | ~7.2M upd/s | a `ModifyOrder` defect off the canonical path is fixed upstream — `RESOLVED_FINDINGS.md` [#42](https://github.com/chronoxor/CppTrader/issues/42) |
| raymondshe (56★) | Rust | with fix | 7.20 | — | MIT-Apache; phantom zero-qty match corrupts next order's id [#1](https://github.com/raymondshe/matchengine-raft/issues/1) |
| Kautenja (309★) | C++ | with fix | 6.88 (normal) | — | reject a duplicate live order-id (no self-linked FIFO / UAF) [#4](https://github.com/Kautenja/limit-order-book/issues/4) |
| ndfex ‡ | C++ | as shipped | 6.825 (swing-25) | — | flat-array book (clean); author: Matthew Belcher (ex-Citadel Securities, 17y HFT) |
| matchcore | Rust | with fix | 6.58 | — | marketable limit passes None → sweeps like market order, pays through own limit [#167](https://github.com/minyukim/matchcore/issues/167) |
| chronex | C++ | with fix | 6.47 | — | MIT; FOK/AON makers fill at aggressor price [#1](https://github.com/OsamaAhmad00/ChroneX/issues/1) |

See [`CONSENSUS_CONFORMING_ENGINES.md`](CONSENSUS_CONFORMING_ENGINES.md) for the full list of
all **160** conforming engines.

### Latency under burst load

Throughput is what an exchange measures internally; what a trader experiences when the market
moves is **latency**, and latency under load is a property of headroom. This table stress-tests
five high-throughput conforming engines, each in **its own weakest scenario**, at offered loads
in the documented microburst range (5–12 M msg/s).[^t7] **All values are nanoseconds,
P50 / P99.**

| Engine | Weakest scenario | 5 M/s | 8 M/s | 12 M/s |
|:-------|:-----------------|:------|:------|:-------|
| FlashOne  | normal       | 354 / 534   | 363 / 568   | 383 / 623 |
| cpp-orderbook | swing-25     | 363 / 2,190     | 457 / 3,309     | 21,400,000 / 33,100,000 † |
| CppTrader     | normal       | 387 / 1,984     | 658 / 3,606     | 23,200,000 / 39,900,000 † |
| Kautenja      | normal       | 428 / 3,070     | 4,740,000 / 17,500,000 † | 45,400,000 / 91,500,000 † |
| asthamishra   | flash-crash  | 496 / 3,153     | 42,400,000 / 59,100,000 † | 90,600,000 / 139,000,000 † |

**† ρ > 1 — the offered load is past the engine's sustainable throughput in that scenario.** The queue grows without bound; therefore, in that case the figure is **not a convergent latency**: it is the median delay accrued over the fixed ~2 M-message burst and rises with burst length.

> **Worst-case stress test.** P50 / P99 are measured from each message's scheduled arrival, open-loop at the stated offered rate, coordinated-omission-free (the queueing delay a slow matcher imposes is never hidden). Every engine eventually diverges as ρ → 1. FlashOne's latency knee sits at ≈ 30 M/s (near-edge P50 ≈ 2.1 µs), and it sustains ≈ 31 M/s.

## Quick start

```sh
git clone <repo-url> matching-engine-benchmark
cd matching-engine-benchmark
make                                            # builds ./harness and ./generator
scripts/build_baselines.sh liquibook quantcup   # two baselines (audit checks one against the other)
./harness --baseline liquibook --scenario normal --mode perf
./harness --baseline liquibook --scenario normal --mode audit
```

`scripts/build_baselines.sh all` builds all three baselines (Exchange-core also
needs a JDK 11 and Maven; see *Requirements*).

To benchmark your own engine algorithm, implement the C ABI in
`api/matching_engine_api.h`, build it as a `.so`, and:

```sh
./harness --engine /path/to/your_engine.so --scenario normal --mode perf
```

See `docs/INTEGRATION.md`.

Compare engines on your own platform:

```sh
scripts/build_baselines.sh all
scripts/run_challenge.py --compare liquibook quantcup exchange_core   # all 5 + worst-case ranking
```

## Why worst-case throughput

A matching engine is the serial heart of an exchange: per-symbol price–time matching is
strictly sequential, so its single-core throughput caps the whole venue.[^serial] Order flow
arrives in microbursts — Deutsche Börse's T7 measurements show inbound flow peaking in the
millions of messages a second at the gateway while only a few hundred thousand a second reach
matching[^t7] — so a burst queues behind the matcher, and that queue, not the average speed,
becomes the worst-case delay in the moments that matter most.[^microburst] The feedback loop
tightens over time: as the exchange gets faster, customers react faster and "send us more
transactions in sharper peaks."[^t7]

When the matcher falls behind, market makers are picked off at stale prices, lose money, and
quote wider and thinner — costs that land on investors.[^armsrace][^marketquality] The
benchmark's definitional result is therefore **worst-case throughput**: the lowest of an
engine's five scenario results, with the scenario that produces it.

*For the plain-language version of this argument and the market-quality evidence, see the
paper's §2.*[^paper]

## Methodology in brief

**Scope.** A single matching engine on one symbol, driven one message at a time through ~2.0M
new/cancel/modify messages (an optional `engine_on_batch` entry point amortises the boundary
cost for engines behind a language runtime — the per-message processing is unchanged). The
engine emits its own report stream (OrderAck / Trade / CancelAck / ModifyAck / CancelReject /
ModifyReject) over an inter-thread transport drained on an adjacent core, so the measured
throughput includes the matcher-to-publisher hand-off a real exchange pays. Multi-core
scaling, networking, and risk checks are out of scope.

**Workload.** A deterministic, realistically shaped equity order flow: GBM mid-price,
power-law depth,[^depth] and a 95% cancel[^cancel] / 15% IOC[^ioc] / 20% modify lifecycle —
the paper benchmark's construction.[^paper] The canonical seed is **23**; the harness
hand-rolls every distribution so a seed reproduces the same workload across compilers
(`docs/METHODOLOGY.md` records why the paper's seed 12345 realises differently here).

**Correctness.** The engine's full report output is hashed (SHA-256) against
`reference/correctness_hash.txt` — the byte-identical consensus first established from three
independent engines (so it favours no single design) and since reproduced by the whole
conforming field.

**A full challenge** is 10 perf runs + 1 audit run per scenario
(`scripts/run_challenge.py`); VALID only if every perf hash and the audit pass. The audit
replays the workload through a baseline engine and probes order-book state at random points;
every run probes the same points, so an engine cannot tell a measured run from an audited
one (`docs/ANTI_CHEAT.md`). By default all five scenarios run and the engine's worst case is
reported (`--scenario` narrows; `--compare` ranks).

**Pre-run conformance gate.** Before any timed run, each engine passes a battery of **33
hand-crafted edge cases**, reverse-engineered from latent bugs found across the survey, plus a
**book-state audit** (best_bid / best_ask / depth checked against the consensus) that catches
a stale or phantom book state that self-heals before any trade and so escapes the report
hash. It tests only hard invariants every correct book must satisfy, never engine-specific
conventions (`docs/CONFORMANCE.md`).

## The rosters

- [`CONSENSUS_CONFORMING_ENGINES.md`](CONSENSUS_CONFORMING_ENGINES.md) — the **160** conforming
  engines (byte-identical on output and book state across 100 random seeds, **1 billion+ order
  messages** each, plus the conformance gate). **as shipped** = conforms unmodified;
  **with fix** = conforms after the minimal documented patch.
- [`NON_CONFORMING_ENGINES.md`](NON_CONFORMING_ENGINES.md) — the **86** whose output diverges,
  or that cannot finish their worst scenario within budget. Non-conforming describes the output
  against the consensus, not engineering quality.
- [`INDUSTRY_AUTHORED_ENGINES.md`](INDUSTRY_AUTHORED_ENGINES.md) — the 52 industry-authored
  engines, with the official/production repos and the production DEX matchers broken out.
- [`CORRECTNESS_FINDINGS.md`](CORRECTNESS_FINDINGS.md) /
  [`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md) — per-engine findings and the fixes already
  merged upstream.

## Repository layout

```
api/                    the C ABI an engine implements
workload/               the deterministic workload generator
src/                    the harness — runner, transport, correctness, audit, platform
adapters/               the three baseline-engine adapters (Liquibook, QuantCup, Exchange-core)
additional_references/  40 worked adapter examples for third-party engines — the permissively-licensed subset of the ~134 adapted across the survey (20+ languages incl. C++/Rust/Go/Java/Python/TypeScript/JavaScript/C#/Zig/C/Haskell/OCaml/Scala/Kotlin/Ruby); the rest are held data-only
patches/                source patches applied to baseline engines (QuantCup)
reference/              the published canonical report output and its hash
scripts/                build_baselines.sh, run_challenge.py, compare_results.py
docs/                   METHODOLOGY, INTEGRATION, ANTI_CHEAT, CONFORMANCE, PATCHES
tests/                  a SHA-256 self-test and three anti-cheat cheat adapters
CORRECTNESS_FINDINGS.md per-engine correctness verdict + filed-issue link, full audited set
CONSENSUS_CONFORMING_ENGINES.md  the conforming roster + worst-case throughput
NON_CONFORMING_ENGINES.md        the non-conforming roster
RESOLVED_FINDINGS.md    findings since fixed upstream (CppTrader, geseq, matchingo, cheetah, OrderBook-rs, matchina, rinok, pyorderbook, dx1ngy)
SNAPSHOTS.md            the pinned upstream commit for every audited engine
```

## Requirements

- Linux, GCC 14+ or Clang 16+ (C++20), CMake 3.16+, Boost headers.
- `scripts/build_baselines.sh` additionally needs `git`; for the Exchange-core
  baseline it needs a JDK 11 and Maven.
- Python 3.8+ for the wrapper scripts.
- The `additional_references/` example adapters pull their own toolchains (Rust,
  Go, a JDK 11+ for the Java/JNI adapters, Node for the TypeScript/JavaScript adapters;
  the Python adapters embed CPython) — each `build.sh` auto-installs the pinned
  toolchain (rustup / go.dev / etc.) if it is absent, which requires `curl` and
  network access. The jxm35 adapter additionally needs a
  C++23 compiler and `libfmt` (`libfmt-dev`).

## FAQ

Q. Why did you build this?

A. To give the community a neutral, reproducible way to measure a matching engine's throughput and verify its correctness on identical work — something that did not exist in the open. The workload, the baseline adapters, and the byte-identical reference hashes are all public, so anyone can run any engine against the same work and the same correctness oracle, on their own hardware. That oracle — the byte-identical consensus that independent open-source engines reproduce, first anchored by three of them — has already surfaced real bugs in several open-source matching engines, including a latent one in a codebase nearly nine years old.

The same openness applies to our own claim: the paper's title — *"The World's Fastest Matching Engine Algorithm"* — is deliberately falsifiable. Beat FlashOne's published numbers on the same work and the title is wrong — a test we are openly inviting. If you reach comparable or higher numbers with your own design, or you think the method is unfair, we would love to hear from you.

Q. Isn't this a self-serving benchmark — any conflict of interest in writing your own benchmark?

A. The test is built in a way that our judgment does not enter it. The correctness reference is the byte-identical consensus that independent open-source engines reproduce — first established from three of them (Liquibook[^liquibook], QuantCup[^quantcup], Exchange-core[^exchangecore]) and since reproduced across the whole conforming field — not our say-so; the workload generator, the adapters, and the reference hashes are public and deterministic, so anyone can independently verify their internal workings. We do not host a leaderboard or rank submissions; you run the harness yourself.

Q. I only want to check that my engine is correct — can I ignore the throughput number?

A. Yes. Run `--mode audit`: it verifies the full report-stream hash against the byte-identical consensus *and* audits the live order book at random points. A `Verdict: VALID` means your engine reproduces the consensus output and maintains a real book, regardless of speed — for many users that correctness signal is the more valuable half.

Q. Isn't a fast matching engine easy to build?

A. Implementing one from the recipes already on the internet is. Inventing new data structures and algorithms that make matching several times faster on the same hardware is not. If writing a fast matching algorithm were easy, industry experts' own engines would routinely reproduce top results; their measured results are catalogued in [`INDUSTRY_AUTHORED_ENGINES.md`](INDUSTRY_AUTHORED_ENGINES.md).

Q. Isn't matcher latency an insignificant part of overall wire-to-wire latency?

A. Yes, on average. But the matcher's throughput headroom — not its average latency — is what shapes the P99 latency curve, and that tail is exactly what a burst exposes. Deutsche Börse (one of Europe's largest exchange operators) makes the point itself: "Our customers are constantly getting faster and send us more transactions in sharper peaks → requires higher throughput on our side."[^t7]

Q. There are a lot of variables the challenge doesn't consider — networking, exchange-specific or per-jurisdiction rules, and the like.

A. By design. The challenge is built to compare *matching engine algorithms* apples-to-apples, not whole *matching engines*. A deployed engine also carries networking, exchange-specific rules, and per-jurisdiction regulatory requirements that differ enormously from venue to venue — so comparing full engines across those differences would mean far less. Holding those layers out of scope is exactly what isolates the algorithm so two of them can be measured on identical work.

## License

MIT — see `LICENSE`. The three baseline engines are fetched from their own
upstream repositories under their own licenses; `docs/PATCHES.md` records every
modification made to build them.

## About Flash One Technologies

We're a team of passionate Math & CS researchers advancing the technological frontier with research-level mathematics. Flash One is a patent IP licensing business, not a matching-engine vendor. We do not offer full matching-engine products. For inquiries, please get in touch with contact@flash1.com.

## References

The market-microstructure claims in *Why worst-case throughput* and the
workload-calibration constants in *Methodology in brief* trace to the benchmark's companion paper and the
primary literature it draws on. Full bibliographic detail (54 references) is in
the paper; the notes below cite the specific sources behind each claim.

[^paper]: Jake Yoon. 2026. *The World's Fastest Matching Engine Algorithm.* Flash One Technologies. [arXiv:2606.01183](https://arxiv.org/abs/2606.01183). The harness, baseline adapters, and byte-identical reference hashes are released with the paper so its results are independently reproducible.
[^microburst]: That order flow concentrates in short, microsecond-scale bursts that dominate the latency tail: Deutsche Börse Group, *Xetra Insights* (2016) and *Insights into Trading System Dynamics: Deutsche Börse's T7* (2025); Albert J. Menkveld, "High-Frequency Trading as Viewed through an Electron Microscope," *Financial Analysts Journal* 74(2), 2018. doi:10.2469/faj.v74.n2.1
[^t7]: Sergej Teverovski (Head of Section, Xetra/Eurex Application Development, Deutsche Börse AG), *T7 — Latency Roadmap*, Deutsche Börse Group Open Day 2023 (21 Sep 2023) — the source of the figures quoted here (857 million daily requests, 13 Mar 2023; 98.6% non-persistent order entry, 4 Aug 2023; and the gateway-to-matching throughput gap — inbound flow peaking in the millions of messages/second at the gateway against a few hundred thousand/second at the start of matching) and of the "T7 latency ↔ customer reaction time" feedback loop and the "constant low latency … in high load situations (aka bursts)" target. <https://www.deutsche-boerse.com/resource/blob/3690194/fe6b01b1e14800eb40374a95516debf2/data/Open%20Day%202023%20-%20Presentation,%20T7-Latency%20Roadmap.pdf>
[^armsrace]: The latency-arbitrage ("stale-quote sniping") race between liquidity providers cancelling stale quotes and fast traders picking them off: Matteo Aquilina, Eric Budish, and Peter O'Neill, "Quantifying the High-Frequency Trading Arms Race," *Quarterly Journal of Economics* 137(1), 2022, 493–564 — they estimate eliminating sniping would cut investors' cost of liquidity by up to ~17%. doi:10.1093/qje/qjab032. See also Eric Budish, Peter Cramton, and John Shim, "The High-Frequency Trading Arms Race: Frequent Batch Auctions as a Market Design Response," *QJE* 130(4), 2015. doi:10.1093/qje/qjv027
[^serial]: Per-symbol matching is strictly serial under price–time priority, so its single-core throughput is a hard ceiling no surrounding parallelism can lift (Gene M. Amdahl, AFIPS 1967, doi:10.1145/1465482.1465560); once a burst exceeds that ceiling, end-to-end latency is governed by queuing delay rather than compute. Publicly documented per-partition matching ceilings are on the order of ~300,000 orders/s (Deutsche Börse T7; Eurex T7 documentation, 2024).
[^marketquality]: Faster matching and lower latency measurably tighten spreads and improve market quality (with diminishing returns once fast access is already available): David M. Kemme, Thomas H. McInish, and Jiang Zhang, "Market fairness and efficiency: Evidence from the Tokyo Stock Exchange," *Journal of Banking & Finance* 134, 2022, 106309 (doi:10.1016/j.jbankfin.2021.106309); Hamish Murray, Thu Phuong Pham, and Harminder Singh, "Latency reduction and market quality: The case of the Australian Stock Exchange," *International Review of Financial Analysis* 46, 2016, 257–265 (doi:10.1016/j.irfa.2015.09.001).
[^cancel]: Modern equity order flow is cancel-dominated — roughly 95% of orders are cancelled and trade-to-order ratios are only a few percent: Marta Khomyn and Tālis J. Putniņš, "Algos gone wild: What drives the extreme order cancellation rates in modern markets?," *Journal of Banking & Finance* 129, 2021, 106170 (doi:10.1016/j.jbankfin.2021.106170); U.S. SEC, *Quote Lifetime Distributions* (2013) and *Trade-to-Order Volume Ratios* (2022).
[^ioc]: The material share of immediate-or-cancel (IOC) liquidity-taking instructions in real order flow: Sida Li, Mao Ye, and Miles Zheng, "Refusing the Best Price?," *Journal of Financial Economics* 147(2), 2023, 317–337. doi:10.1016/j.jfineco.2022.11.004
[^depth]: Heavy-tailed / power-law limit-order-book depth profiles: Jean-Philippe Bouchaud, Marc Mézard, and Marc Potters, "Statistical properties of stock order books," *Quantitative Finance* 2, 2002; Rama Cont, Sasha Stoikov, and Rishi Talreja, "A stochastic model for order book dynamics," Columbia University, 2010; Martin D. Gould et al., "Limit order books," *Quantitative Finance* 13(11), 2013; Ilija I. Zovko and J. Doyne Farmer, "The power of patience: A behavioral regularity in limit-order placement," *Quantitative Finance* 2(5), 2002.
[^liquibook]: Object Computing, Inc. 2013. *Liquibook: An Open Source C++ Order Matching Engine.* <https://github.com/ObjectComputing/liquibook>
[^quantcup]: QuantCup 1 Contest, 2011 — *Price–Time Matching Engine* (winning entry), sponsored by Tower Research Capital. Original C entry: <https://gist.github.com/druska/d6ce3f2bac74db08ee9007cdf98106ef>; the harness builds the C++/Boost port at <https://github.com/ajtulloch/quantcup-orderbook> (the upstream pinned in `docs/PATCHES.md`).
[^exchangecore]: Maksim Zheravin. 2019. *Exchange-core: Ultra-fast matching engine.* <https://github.com/exchange-core/exchange-core>
