# Matching Engine Algorithm Performance Challenge

A reproducible benchmark for limit-order-book matching engine algorithms. Published with
[*"The World's Fastest Matching Engine Algorithm"*](https://arxiv.org/abs/2606.01183) (Flash One Technologies, 2026).[^paper]

The harness replays a fixed, deterministic order-flow workload through an engine algorithm 
loaded as a shared library, verifies the result against a cryptographic hash,
and measures throughput — so any two engine algorithms (hereafter "engine") can be compared on identical work.

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

## Background

A stock exchange runs on one piece of software called the *matching engine* —
the part that pairs up incoming buy and sell orders and turns them into
trades. It is the heart of the market, and how fast it works sets the speed
limit for the whole venue. Trading doesn't arrive in a steady stream; it comes
in bursts — the market sits quiet, then the instant a price moves or news
breaks everyone reacts at once and a flood of order messages lands within a
few millionths of a second.[^microburst] Most aren't trades at all but quotes
that automated traders post and pull as they reprice: on Deutsche Börse's
exchange **98.6%** of order-entry traffic is these throwaway, non-resting
quotes, and the system has handled **857 million** requests in a single
day.[^t7]

Speed creates its own problem. The faster the exchange runs, the faster its
customers can react — so they fire the next message sooner and the bursts grow
sharper still; Deutsche Börse describes exactly this feedback loop, with
*exchange latency* and *customer reaction time* chasing each other downward as
customers "are constantly getting faster and send us more transactions in
sharper peaks."[^t7] Underneath every price move a race plays out: market
makers scrambling to cancel quotes that have just gone stale before someone
trades against them at the old price, and faster traders scrambling to hit
those same quotes first.[^armsrace]

This is why a matching engine is judged on its *throughput*, not its average
speed. On a calm day almost anything is fast enough; what matters is
**headroom** — how big a burst it can swallow before a queue forms behind it.
The exchange's own goal is "constant low latency especially in high load
situations (aka bursts)":[^t7] keep pace and delays stay flat; fall behind and
every message in the burst waits in line, and that queue — not the average
speed — is what becomes the worst-case delay (the "P99", the slowest 1 in 100)
in the moments that matter most.[^serial] When the engine can't clear a burst,
market makers get picked off at stale prices, lose money, and quote more
cautiously — wider spreads, less size — so the market turns thin and expensive
and the ordinary investor quietly pays the difference.[^marketquality]

## What it measures

A single matching engine on one symbol. The harness drives it one message at a
time through ~2.0M new/cancel/modify messages; the **engine emits its own
report stream** (OrderAck / Trade / CancelAck / ModifyAck / CancelReject /
ModifyReject) over an inter-thread transport drained on an adjacent core — so
the measured throughput includes the
matcher-to-publisher hand-off a real exchange pays. The engine may use threads:
the harness measures it running its real production architecture.

It does not measure multi-core scaling, networking, or risk checks; those are
out of scope.

## How a run works

`./harness` runs in one of two modes:

- **perf** — times the workload and reports throughput; verifies the output hash.
- **audit** — replays the workload through a baseline engine and compares
  order-book state at random points (anti-cheat); not timed.

A full challenge is **10 perf runs + 1 audit run** per scenario, driven by
`scripts/run_challenge.py`. The result is VALID only if every perf hash passes
and the audit passes. Separating the passes keeps the measured runs free of any
anti-cheat overhead while still validating the book. See `docs/ANTI_CHEAT.md`.

## The reference engines

Three reference engines span the design space — Liquibook[^liquibook] (tree of lists),
QuantCup[^quantcup] (flat price-indexed array), and Exchange-core[^exchangecore] (direct-access order
book on the JVM). Each has a distinct failure mode, which is the point of the
five scenarios: QuantCup's flat array is fastest while prices stay in a
narrow band (`static`) and collapses ~15× as the walk spreads
(`flash-crash`); Liquibook's node-per-order multimap is the opposite —
mildly volatility-sensitive, but it pays per resting order and collapses
under `static`'s ~21,000-order standing book; Exchange-core is roughly flat
but pays a JNI crossing per message. All three nonetheless produce a
byte-identical output stream — every report, not just trades — and that
agreement is the correctness reference.

Throughput on the canonical workload (median of 10 trials, single matcher /
single drainer on adjacent cores, Graviton4 / Neoverse-V2,
`-O3 -march=native`):

| Engine          | static    | normal    | swing-25  | swing-40  | flash-crash |
|-----------------|----------:|----------:|----------:|----------:|------------:|
| Liquibook       | infeasible | 4.33 M/s  | 4.61 M/s  | 4.82 M/s  | 4.86 M/s    |
| QuantCup        | 8.50 M/s  | 6.93 M/s  | 1.60 M/s  | 0.94 M/s  | 0.57 M/s    |
| Exchange-core   | 1.51 M/s  | 1.56 M/s  | 1.32 M/s  | 1.31 M/s  | 1.20 M/s    |
| **FlashOne**    | **40.95 M/s** | **31.09 M/s** | **31.41 M/s** | **31.81 M/s** | **32.69 M/s** |

FlashOne is the harness publisher's production engine — included as a
published reference; its `.so` is not publicly available, so the row is
reproducible only under a production license. See `discoveries.md` for
per-engine architecture notes, observations from eleven further surveyed
engines, and how to interpret each row.

**Order-identifier tracking.** On every cancel, modify, and fill an engine
must map the harness's client order id back to its own internal order handle.
So that this step is measured apples-to-apples against engines that allocate
their order tables statically — QuantCup, for instance, indexes a flat
price array directly — the reference adapters perform that translation with
flat-array direct indexing rather than a hash map (no adapter-side locks, no
per-message allocation; each adapter README documents its mapping). Flat
indexing is also the
more faithful model of real order-entry protocols, where a session assigns
increasing, dense order identifiers — the sequential UserRefNum discipline of
Nasdaq OUCH 5.0[^ouch] — that a direct index serves exactly.

Measure on your own platform:

```sh
scripts/build_baselines.sh all
scripts/run_challenge.py --compare liquibook quantcup exchange_core --all-scenarios
```

### Surveyed engines vs. their published figures

Beyond the three calibration baselines, the harness has been run against the
eleven third-party engines in `additional_references/` — each selected for
>100 GitHub stars, a published >10 M orders/sec claim, or wide use as a
teaching reference, and wrapped by a worked adapter. The figure each project
publishes was measured under its own workload and its own definition of an
operation — typically a single-threaded, in-process, single-symbol
micro-benchmark with no cancels or modifies and no inter-thread report drain.
The harness column is the same engine under one realistic workload: ~95% cancel
/ 15% IOC, a GBM mid-price walk with a standing book the engine must carry,
and every report drained to a separate core.
The two are **not** like-for-like; the table records both so the difference is
visible (per-engine conditions and correctness findings are in
`discoveries.md`).

| Engine       | Harness `normal` (full report drainage) | Project's published figure |
|--------------|----------------------------------------:|---------------------------:|
| piyush       | 4.67 M/s (INVALID — state audit)        | ~160 M/s                   |
| philipgreat  | 4.43 M/s (VALID with fix — 3 engine correctness patches) | ~125 M/s ("8 ns/order")    |
| limitbook    | 2.72 M/s (INVALID — over-match)         | ~30 M/s                    |
| robaho       | 3.02 M/s (INVALID — price field)        | 10–22 M/s                  |
| geseq        | infeasible (>60 s/trial; VALID with fix in audit — engine price-predicate patch) | 12.5–21 M/s                |
| mansoor      | 0.03 M/s                                | >20 M/s                    |
| jxm35        | 2.20 M/s (INVALID — untraced)           | 14 M/s                     |
| femto_go     | infeasible (>60 s/trial)                | >10 M/s                    |
| CppTrader    | 7.26 M/s (VALID on canonical; INVALID on an off-canonical deeper-book dev stress variant — order-index corruption) | ~3.2 M/s                   |
| OrderBook-rs | 0.56 M/s (INVALID — priority only)      | latency-focused            |
| Tzadiko      | 3.45 M/s (VALID with fix — engine deadlock patch) | not headlined              |

Every engine that advertises a double-digit-million-per-second (or higher)
figure lands in single digits of M/s — or does not complete — once reports
cross a thread boundary under a cancel-heavy workload with a standing book to
carry; FlashOne sustains 31.1–41.0 M/s across the same five scenarios
(above). Correctness separates the field further: of the eleven, only CppTrader,
Tzadiko (after an engine deadlock patch), philipgreat (after three engine
correctness patches), geseq (after one), femto_go, and mansoor reproduce the
byte-identical consensus everywhere they run; limitbook over-matches (a
quantity-conservation violation), OrderBook-rs and robaho each differ in a
single field (counterparty identity and execution price respectively, with
quantities correct), jxm35 diverges for reasons we did not trace to source,
and piyush fails the state audit on the moving scenarios. See
`discoveries.md` for the specific findings and the VALID / INVALID
criterion.

## How it works

- **Workload** — a deterministic, realistically shaped equity order flow: a
  geometric-Brownian-motion mid-price, power-law order depth,[^depth] and a 95% cancel[^cancel] /
  15% IOC[^ioc] / 20% modify lifecycle with occasional duplicate cancels and
  modifies — the paper benchmark's construction.[^paper] The canonical seed was
  changed from 12345 to **23**: the harness hand-rolls every distribution so a
  seed reproduces the same workload across compilers (the paper's `std::`
  distributions do not), and that difference makes a given seed realise a
  different resting book here than in the paper — the old seed-12345 run left
  the `normal` book nearly empty, whereas the paper's carries ~5,300 resting
  orders at the end of the run. Seed 23 is the realisation that reproduces the
  paper's standing-book profile scenario for scenario (see
  `docs/METHODOLOGY.md`).
- **Correctness** — the engine's full report output is hashed (SHA-256) and
  checked against `reference/correctness_hash.txt`, which ships a hash for
  every scenario at the canonical seed (23). The canonical entry — `normal` + seed 23 —
  is the byte-identical consensus of all three baseline engines, so it favours
  no single design. `docs/METHODOLOGY.md`.
- **Anti-cheat** — a perf run checks the full report-stream hash; one audit
  run replays the workload through a baseline engine and runs a random-point
  order-book state audit. Every run probes the book at the same random points,
  so an engine cannot tell a measured run from an audited one.
  `docs/ANTI_CHEAT.md`.

## Repository layout

```
api/                    the C ABI an engine implements
workload/               the deterministic workload generator
src/                    the harness — runner, transport, correctness, audit, platform
adapters/               the three baseline-engine adapters (Liquibook, QuantCup, Exchange-core)
additional_references/  eleven worked adapter examples for third-party engines (C++, Rust, Go)
patches/                source patches applied to baseline engines (QuantCup)
reference/              the published canonical report output and its hash
scripts/                build_baselines.sh, run_challenge.py, compare_results.py
docs/                   METHODOLOGY, INTEGRATION, ANTI_CHEAT, PATCHES
tests/                  a SHA-256 self-test and three anti-cheat cheat adapters
discoveries.md          observations the harness produced against the eleven surveyed engines
```

## Requirements

- Linux, GCC 14+ or Clang 16+ (C++20), CMake 3.16+, Boost headers.
- `scripts/build_baselines.sh` additionally needs `git`; for the Exchange-core
  baseline it needs a JDK 11 and Maven.
- Python 3.8+ for the wrapper scripts.
- The `additional_references/` example adapters pull their own toolchains: the
  three Rust adapters need Rust and the two Go adapters need Go — each `build.sh`
  auto-installs the pinned toolchain (rustup / go.dev) if it is absent, which
  requires `curl` and network access. The jxm35 adapter additionally needs a
  C++23 compiler and `libfmt` (`libfmt-dev`).

## FAQ

Q. Why do you do this?

A. To give anyone a concrete, reproducible way to *falsify* the paper's title — *"The World's Fastest Matching Engine Algorithm."* The workload, the baseline adapters, and the byte-identical reference hashes are all public, so any engine can be run on identical work and measured against FlashOne's published numbers. Beat them on the same work and the title is wrong; that is the test we are openly inviting. If you ever hit comparable or higher numbers with your own engine design, we would love to know. If you think the testing method is unfair, we would love to know that too.

Q. Isn't a fast matching engine easy to build?

A. Implementing one from the recipes already on the internet is. Inventing new data structures and algorithms that make matching several times faster on the same hardware is not. Almost every implementation is a variant of the same idea — a linked-list order queue inside a pre-allocated, statically allocated contiguous region — yet, before our work, no one had pinned down the optimal size of that region or how those links are best implemented.

Q. Isn't matcher latency an insignificant part of overall wire-to-wire latency?

A. Yes, on average. But the matcher's throughput headroom — not its average latency — is what shapes the P99 latency curve, and that tail is exactly what a burst exposes. For the full argument on why the matcher needs throughput headroom several times its average load, see the Background above and the paper.

Q. There are a lot of variables the challenge doesn't consider — networking, exchange-specific or per-jurisdiction rules, and the like.

A. By design. The challenge is built to compare *matching engine algorithms* apples-to-apples, not whole *matching engines*. A deployed engine also carries networking, exchange-specific rules, and per-jurisdiction regulatory requirements that differ enormously from venue to venue — so comparing full engines across those differences would mean far less. Holding those layers out of scope is exactly what isolates the algorithm so two of them can be measured on identical work.

## License

MIT — see `LICENSE`. The three baseline engines are fetched from their own
upstream repositories under their own licenses; `docs/PATCHES.md` records every
modification made to build them.

## References

The market-microstructure claims in *Background* and the workload-calibration
constants in *How it works* trace to the benchmark's companion paper and the
primary literature it draws on. Full bibliographic detail (54 references) is in
the paper; the notes below cite the specific sources behind each claim.

[^paper]: Jake Yoon. 2026. *The World's Fastest Matching Engine Algorithm.* Flash One Technologies. [arXiv:2606.01183](https://arxiv.org/abs/2606.01183). The harness, baseline adapters, and byte-identical reference hashes are released with the paper so its results are independently reproducible.
[^microburst]: That order flow concentrates in short, microsecond-scale bursts that dominate the latency tail: Deutsche Börse Group, *Xetra Insights* (2016) and *Insights into Trading System Dynamics: Deutsche Börse's T7* (2025); Albert J. Menkveld, "High-Frequency Trading as Viewed through an Electron Microscope," *Financial Analysts Journal* 74(2), 2018. doi:10.2469/faj.v74.n2.1
[^t7]: Sergej Teverovski (Head of Section, Xetra/Eurex Application Development, Deutsche Börse AG), *T7 — Latency Roadmap*, Deutsche Börse Group Open Day 2023 (21 Sep 2023) — the source of the figures quoted here (857 million daily requests, 13 Mar 2023; 98.6% non-persistent order entry, 4 Aug 2023) and of the "T7 latency ↔ customer reaction time" feedback loop and the "constant low latency … in high load situations (aka bursts)" target. <https://www.deutsche-boerse.com/resource/blob/3690194/fe6b01b1e14800eb40374a95516debf2/data/Open%20Day%202023%20-%20Presentation,%20T7-Latency%20Roadmap.pdf>
[^armsrace]: The latency-arbitrage ("stale-quote sniping") race between liquidity providers cancelling stale quotes and fast traders picking them off: Matteo Aquilina, Eric Budish, and Peter O'Neill, "Quantifying the High-Frequency Trading Arms Race," *Quarterly Journal of Economics* 137(1), 2022, 493–564 — they estimate eliminating sniping would cut investors' cost of liquidity by up to ~17%. doi:10.1093/qje/qjab032. See also Eric Budish, Peter Cramton, and John Shim, "The High-Frequency Trading Arms Race: Frequent Batch Auctions as a Market Design Response," *QJE* 130(4), 2015. doi:10.1093/qje/qjv027
[^serial]: Per-symbol matching is strictly serial under price–time priority, so its single-core throughput is a hard ceiling no surrounding parallelism can lift (Gene M. Amdahl, AFIPS 1967, doi:10.1145/1465482.1465560); once a burst exceeds that ceiling, end-to-end latency is governed by queuing delay rather than compute. Publicly documented per-partition matching ceilings are on the order of ~300,000 orders/s (Deutsche Börse T7; Eurex T7 documentation, 2024).
[^marketquality]: Faster matching and lower latency measurably tighten spreads and improve market quality (with diminishing returns once fast access is already available): David M. Kemme, Thomas H. McInish, and Jiang Zhang, "Market fairness and efficiency: Evidence from the Tokyo Stock Exchange," *Journal of Banking & Finance* 134, 2022, 106309 (doi:10.1016/j.jbankfin.2021.106309); Hamish Murray, Thu Phuong Pham, and Harminder Singh, "Latency reduction and market quality: The case of the Australian Stock Exchange," *International Review of Financial Analysis* 46, 2016, 257–265 (doi:10.1016/j.irfa.2015.09.001).
[^cancel]: Modern equity order flow is cancel-dominated — roughly 95% of orders are cancelled and trade-to-order ratios are only a few percent: Marta Khomyn and Tālis J. Putniņš, "Algos gone wild: What drives the extreme order cancellation rates in modern markets?," *Journal of Banking & Finance* 129, 2021, 106170 (doi:10.1016/j.jbankfin.2021.106170); U.S. SEC, *Quote Lifetime Distributions* (2013) and *Trade-to-Order Volume Ratios* (2022).
[^ioc]: The material share of immediate-or-cancel (IOC) liquidity-taking instructions in real order flow: Sida Li, Mao Ye, and Miles Zheng, "Refusing the Best Price?," *Journal of Financial Economics* 147(2), 2023, 317–337. doi:10.1016/j.jfineco.2022.11.004
[^depth]: Heavy-tailed / power-law limit-order-book depth profiles: Jean-Philippe Bouchaud, Marc Mézard, and Marc Potters, "Statistical properties of stock order books," *Quantitative Finance* 2, 2002; Rama Cont, Sasha Stoikov, and Rishi Talreja, "A stochastic model for order book dynamics," Columbia University, 2010; Martin D. Gould et al., "Limit order books," *Quantitative Finance* 13(11), 2013; Ilija I. Zovko and J. Doyne Farmer, "The power of patience: A behavioral regularity in limit-order placement," *Quantitative Finance* 2(5), 2002.
[^ouch]: Nasdaq OUCH 5.0 order-entry protocol: a session assigns each order a `UserRefNum`, a day-unique identifier the protocol requires to be supplied in strictly increasing order — i.e. dense and sequential per session — which a flat direct index resolves in O(1) without a hash map.
[^liquibook]: Object Computing, Inc. 2013. *Liquibook: An Open Source C++ Order Matching Engine.* <https://github.com/ObjectComputing/liquibook>
[^quantcup]: QuantCup 1 Contest, 2011 — *Price–Time Matching Engine* (winning entry), sponsored by Tower Research Capital. Original C entry: <https://gist.github.com/druska/d6ce3f2bac74db08ee9007cdf98106ef>; the harness builds the C++/Boost port at <https://github.com/ajtulloch/quantcup-orderbook> (the upstream pinned in `docs/PATCHES.md`).
[^exchangecore]: Maksim Zheravin. 2019. *Exchange-core: Ultra-fast matching engine.* <https://github.com/exchange-core/exchange-core>
