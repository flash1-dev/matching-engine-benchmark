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
breaks, everyone reacts at once and a flood of order messages lands within a
few millionths of a second.[^microburst]

Speed creates its own problem. The faster the exchange runs, the faster its
customers can react — so they fire the next message sooner and the bursts grow
sharper still; Deutsche Börse describes exactly this feedback loop, with
*exchange latency* and *customer reaction time* chasing each other downward as
customers "are constantly getting faster and send us more transactions in
sharper peaks."[^t7] That feedback loop has only tightened as ever-faster
hardware — and, lately, AI-driven trading — lets participants react faster
still. Underneath every price move a race plays out: market makers scrambling
to cancel quotes that have just gone stale before someone trades against them
at the old price, and faster traders scrambling to hit those same quotes
first.[^armsrace]

This is why a matching engine is judged on its *throughput*, not its average
speed. On a calm day almost anything is fast enough; what matters is
**headroom** — how big a burst it can swallow before a queue forms behind it.
The matcher is typically the narrowest stage of the whole pipeline: a venue's
gateways and sequencers can take in far more traffic than the strictly serial
match loop can clear — Deutsche Börse's T7 shows inbound flow peaking in the
millions of messages a second at the gateway while only a few hundred thousand
a second reach matching — so a burst piles up behind the matcher, not upstream
of it.[^t7]

The exchange's own goal is "constant low latency especially in high load
situations (aka bursts)":[^t7] keep pace and delays stay flat; fall behind and
every message in the burst waits in line, and that queue — not the average
speed — is what becomes the worst-case delay (the "P99", the slowest 1 in 100)
in the moments that matter most.[^serial] When the engine can't clear a burst,
**market makers get picked off at stale prices, lose money, and quote more
cautiously** — wider spreads, less size — so the market turns thin and expensive
and the ordinary investor quietly pays the difference.[^marketquality]

## What it measures

A single matching engine on one symbol. The harness drives it one message at a
time through ~2.0M new/cancel/modify messages; one-at-a-time is the default, but
an engine may export the optional `engine_on_batch` entry point so the harness
delivers messages in runs (one boundary crossing per run; per-message processing
unchanged) — recommended for engines behind a language runtime (Go/cgo, Java/JNI)
so the boundary cost is amortized rather than measured (`docs/METHODOLOGY.md`).
The **engine emits its own
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
By default `run_challenge.py` runs all five scenarios and reports the engine's
**worst-case** throughput — the lowest of the five, with the scenario that
produces it — as its definitional result, since a venue must survive its worst
regime, not its best (`--scenario` narrows to one; `--compare` ranks engines by
worst case).

## The reference engines

Three reference engines span the design space — Liquibook[^liquibook] (tree of lists),
QuantCup[^quantcup] (flat price-indexed array), and Exchange-core[^exchangecore] (direct-access order
book on the JVM). Each has a distinct failure mode, which is the point of the
five scenarios: QuantCup's flat array is fastest while prices stay in a
narrow band (`static`) and collapses ~15× as the walk spreads
(`flash-crash`); Liquibook's node-per-order multimap is the opposite —
mildly volatility-sensitive, but it pays per resting order and collapses
under `static`'s ~21,000-order standing book; Exchange-core is roughly flat
but crosses into the JVM per message — a JNI cost the harness's batch delivery
amortizes (`docs/METHODOLOGY.md`). All three nonetheless produce a
byte-identical output stream — every report, not just trades — and that
agreement is the correctness reference.

Throughput on the canonical workload (median of 10 trials, single matcher /
single drainer on adjacent cores, Graviton4 / Neoverse-V2, `-O3 -march=native`),
ranked by **worst-case throughput** — the lowest of an engine's five scenario
results — because a venue must survive its worst regime, not its best. The
scenario that produces each worst case is shown alongside:

| Engine        | Worst-case throughput | Weakest scenario |
|---------------|----------------------:|:-----------------|
| FlashOne      | 33.20 M/s             | `normal`         |
| Exchange-core | 1.40 M/s              | `flash-crash`    |
| QuantCup      | 0.57 M/s              | `flash-crash`    |
| Liquibook     | 0.03 M/s              | `static`         |

FlashOne is the harness publisher's production engine, shown as a reference.
It stands only as a target to beat on fully public, audited work: the harness,
baselines, workload, and hashes that define that target are all open, so anyone can try.
See `discoveries.md` for per-engine architecture notes, the eleven further surveyed engines,
and how to interpret each row.

Measure on your own platform:

```sh
scripts/build_baselines.sh all
scripts/run_challenge.py --compare liquibook quantcup exchange_core   # all 5 + worst-case ranking
```

### Surveyed engines vs. their published figures

Beyond the three calibration baselines, the harness has been run against the
eleven third-party engines in `additional_references/` — each selected for
>100 GitHub stars, a published >10 M orders/sec claim, or wide use as a
teaching reference, and wrapped by a worked adapter. Rows are ordered by each project's published claim, highest first.

| Engine       | Harness worst-case (weakest scenario)   | Project's published figure |
|:-------------|:----------------------------------------|---------------------------:|
| piyush       | 3.17 M/s, `flash-crash` (INVALID — state audit) | ~160 M/s |
| philipgreat  | 0.03 M/s, `static` (VALID with fix — 3 engine correctness patches) | ~125 M/s ("8 ns/order") |
| limitbook    | 1.15 M/s, `static` (INVALID — over-match) | ~30 M/s |
| robaho       | 1.89 M/s, `swing-25` (INVALID — price field) | 10–22 M/s |
| geseq        | 1.57 M/s, `static` (VALID with fix — engine price-predicate patch) | 12.5–21 M/s |
| mansoor      | 0.03 M/s, `normal` (VALID ×5) | >20 M/s |
| jxm35        | 2.20 M/s, `normal` (INVALID — untraced) | 14 M/s |
| femto_go     | 2.24 M/s, `normal` (VALID on `static`/`normal`; INVALID on others) | >10 M/s |
| CppTrader    | 7.26 M/s, `normal` (VALID ×5) | ~3.2 M/s |
| OrderBook-rs | 0.13 M/s, `static` (INVALID — priority only) | latency-focused |
| Tzadiko      | 3.39 M/s, `flash-crash` (VALID with fix — engine deadlock patch) | not headlined |

These findings are offered back, not aimed at anyone. The findings are factual and no judgment has been made for each project. Each is a reproducible,
time-stamped *snapshot* of a specific commit — not a verdict on a project's
quality — and several ship with a fix the reference adapter applies
(`discoveries.md` documents every patch). One — a CppTrader `ModifyOrder` crash —
was reported upstream and fixed; its history is in `RESOLVED_FINDINGS.md`.

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

Q. Why did you build this?

A. To give the community a neutral, reproducible way to measure a matching engine's throughput and verify its correctness on identical work — something that did not exist in the open. The workload, the baseline adapters, and the byte-identical reference hashes are all public, so anyone can run any engine against the same work and the same correctness oracle, on their own hardware. That oracle — the byte-identical consensus of three independent reference engines — has already surfaced real bugs in several open-source matching engines, including a latent one in a codebase nearly nine years old.

The same openness applies to our own claim: the paper's title — *"The World's Fastest Matching Engine Algorithm"* — is deliberately falsifiable. Beat FlashOne's published numbers on the same work and the title is wrong — a test we are openly inviting. If you reach comparable or higher numbers with your own design, or you think the method is unfair, we would love to hear from you.

Q. Isn't this a self-serving benchmark — any conflict of interest in writing your own benchmark?

A. The test is built in a way that our judgment does not enter it. The correctness reference is the byte-identical agreement of three independent open-source engines (Liquibook, QuantCup, Exchange-core), not our say-so; the workload generator, the adapters, and the reference hashes are public and deterministic, so anyone can independently verify their internal workings. We do not host a leaderboard or rank submissions; you run the harness yourself.

Q. I only want to check that my engine is correct — can I ignore the throughput number?

A. Yes. Run `--mode audit`: it verifies the full report-stream hash against the three-engine consensus *and* audits the live order book at random points. A `Verdict: VALID` means your engine reproduces the consensus output and maintains a real book, regardless of speed — for many users that correctness signal is the more valuable half.

Q. Isn't a fast matching engine easy to build?

A. Implementing one from the recipes already on the internet is. Inventing new data structures and algorithms that make matching several times faster on the same hardware is not. Almost every public implementation is a variant of the same idea — a linked-list order queue inside a statically allocated contiguous region — yet, before our work, no one had pinned down the optimal size of that region or how those links are best implemented.

Q. Isn't matcher latency an insignificant part of overall wire-to-wire latency?

A. Yes, on average. But the matcher's throughput headroom — not its average latency — is what shapes the P99 latency curve, and that tail is exactly what a burst exposes. Deutsche Börse (one of Europe's largest exchange operators) makes the point itself: "Our customers are constantly getting faster and send us more transactions in sharper peaks → requires higher throughput on our side."[^t7]

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
