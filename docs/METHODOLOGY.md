# Methodology

This document describes the workload the harness replays, the report stream the
engine emits, how throughput is measured, and how correctness is verified. The
implementation of record is `workload/generator.cpp`; the constants below are
compiled into it and mirrored in `workload/scenarios.json`.

## Workload model

The workload is a synthetic but realistically-shaped order flow for a single
liquid US equity, calibrated to NVDA. One run is 1,000,000 new orders, which
expand into ~2.0M timeline messages once cancels, modifies, and their
occasional duplicates are interleaved. Every empirical constant below is
calibrated to the U.S. equity-microstructure literature or to SEC data; see
*References* at the end of this document.

### Price grid

- Tick size: **$0.005** — the SEC half-penny minimum pricing increment for NMS
  stocks priced ≥ $1.00 with a time-weighted average quoted spread ≤ $0.015,
  adopted in *Amendments to Rule 612 of Regulation NMS*, release 34-101070,
  with a November 3, 2025 compliance date [secRule612_2024].
- Starting mid-price: **$167.52**, i.e. **33,504 ticks**.
- Prices are signed integer ticks throughout; the ABI never sees dollars.

### Mid-price path

The mid-price follows geometric Brownian motion, one step per new order:

    mid(t+1) = mid(t) * exp(-0.5 * sigma^2 * dt + sigma * sqrt(dt) * Z)

`Z` is a standard normal, `sigma` is the scenario's annualised volatility, and
the step `dt = (swing / sigma)^2 / N` is chosen so the path's typical excursion
over the run is the scenario's target swing. GBM is a standard construction
and is not attributed to a single source; for the embedding of GBM into a
stochastic limit-order-book see [cont2010stochastic].

### Order placement

Each new order is, with probability 0.05, **marketable** — priced 1–4 ticks
through the mid into the opposite side so it crosses resting liquidity. The
other 95% **rest**: their distance from the mid is drawn from a power-law depth
profile (exponent alpha = 2.23) with a Gaussian "hump" about 8 ticks out,
reproducing the near-touch liquidity concentration of real books. Quantity is
uniform on [1, 100]. The power-law decay of update intensity per depth level
(`#updates(ℓ) ∝ ℓ^−β`, `β > 1`) and the roughly exponential decay of resting
queue length with distance from the touch are the empirical regularities
documented across [bouchaud2002orderbook; gould2013lob; zovkofarmer2002patience;
cont2010stochastic; munitoke2013queueing].

### Order lifecycle

The placed orders are shuffled into arrival order and a lifecycle is expanded
around each:

- **15%** of new orders are **immediate-or-cancel** — they match what they can
  and any residual is cancelled. IOC orders are never modified or re-cancelled.
  The 15% share reflects the material role of liquidity-taking IOC-like
  instructions in modern U.S. equity flow [liYeZheng2023].
- Of the non-IOC orders, **20%** receive a **modify** (a quantity increase, and
  ~80% of the time also a 1-tick reprice).
- **95%** of non-IOC orders are eventually **cancelled**; the lifetime is
  exponential with a median of **0.431 ms**, and deeper resting orders live
  longer. The cancellation dominance is consistent with the low single-digit
  trade-to-order ratios in SEC market-quality data [secTradeToOrderVolume2022]
  and the order-to-trade-ratio literature [khomynPutnins2021]; the
  sub-millisecond lifetime is calibrated to the SEC's quote-life studies
  [secQuoteLife2013; secQuoteLifeCondFreq2025].
- A real trading system re-sends a cancel or an amend to be sure it lands.
  **~2%** of cancels are followed by a **duplicate cancel**, and **~2%** of the
  orders that were modified and then cancelled get a **duplicate (stale)
  modify** — each arriving after the order is already gone. The engine answers
  these with a CancelReject / ModifyReject (see *Report stream*); they stay a
  few percent of all cancel / modify messages.

Messages are stable-sorted by synthetic timestamp into wire order, then each is
assigned a dense, 0-based `sequence_number`.

### The five scenarios

| Scenario | Annualised vol | Target swing | Character |
|---|---:|---:|---|
| `static` | 0.00 | 0% | Fixed mid; isolates data-structure cost. Still crosses and trades via the marketable fraction. |
| `normal` | 0.15 | 2% | Routine intraday session. **Canonical** — the byte-identical three-baseline consensus is anchored on `normal` + seed 12345. |
| `swing-25` | 0.50 | 25% | High-volatility day. |
| `swing-40` | 0.50 | 40% | Stressed market. |
| `flash-crash` | 0.50 | 60% | Flash-crash dislocation, after documented intraday events such as the May 6, 2010 U.S. equity flash crash. |

Every scenario produces trades; none is insert-only.

## Determinism

The published correctness hash must be reproducible bit-for-bit on any compiler
and platform. Only `std::mt19937` is bit-portable across C++ standard libraries
— `std::normal_distribution`, `std::discrete_distribution`, and the
distributions inside `std::shuffle` are not. The generator therefore hand-rolls
every distribution (uniform, normal via Box–Muller, exponential, the power-law
CDF, and the order-of-arrival shuffle) directly on `std::mt19937`. The same
`(scenario, count, seed)` yields a byte-identical workload everywhere.

`std::mt19937` is seeded by a 32-bit value, so `--seed` accepts values in
`[0, 2^32 − 1]`. The generator refuses larger seeds rather than silently
truncating, since two callers asking for "the seed-X workload" must always
get the same bytes regardless of which 64-bit container the harness happens
to use. (Floating-point transcendentals — `log`, `cos`, `pow`, `exp` — are
sourced from the host libm. IEEE-754 requires only `sqrt` to be correctly
rounded; the others can differ by up to 1 ULP between glibc, musl, Apple
libm, and UCRT. In practice the published hashes are reproduced on
glibc + libstdc++ x86_64 and aarch64; other targets may need a host-specific
regeneration of `reference/correctness_hash.txt`.)

## On-disk format

`orders_<scenario>_seed<seed>_count<count>.bin`: a 16-byte header
`[u64 magic][u32 version][u32 count]` then `count` fixed 40-byte records:

    [u8 type][u8 side][u8 ioc][u8 pad][u32 quantity]
    [u64 sequence_number][u64 order_id][i64 price_ticks][i64 reserved]

`type` is 0 = NEW, 1 = CANCEL, 2 = MODIFY. The (scenario, seed, count) triple
is encoded in the filename so the harness cannot silently reuse a stale
workload when any of the three inputs change between runs. This file is a
harness-internal cache; it is not part of the engine ABI.

## Report stream

A production matching engine hands every result to an outbound publisher across
a thread boundary, and that hand-off is a real cost. The harness models it: the
**engine emits its own report stream** and pushes it through an inter-thread
transport — a single-producer / single-consumer queue drained by a separate
thread pinned to an adjacent core. The push is inside the measured window. The
harness does not synthesize reports; it only drains and counts what the engine
produces.

- **OrderAck** — one per accepted new order.
- **Trade** — one per fill; the fill price is the **maker's** (resting order's)
  price and the **aggressive** order's `sequence_number`.
- **CancelAck** — one per successful cancel, and one per IOC residual
  cancellation (the unfilled remainder of an IOC order).
- **ModifyAck** — one per successful modify.
- **CancelReject** — one per cancel of an order that is not resting (already
  filled, already cancelled, or never seen) — the production "too late to
  cancel" / "unknown order" response.
- **ModifyReject** — one per modify of an order that is not resting.

An engine may supply its own transport (see `docs/INTEGRATION.md`); by default
the harness provides a `boost::lockfree::spsc_queue`. The transport choice never
affects correctness — only how reports are carried between threads.

Because the engine matches and reports on whatever thread(s) it likes, the
harness calls `engine_flush()` after the last message — inside the timed window
— and the call blocks until every message is fully matched and every report
emitted. Deferred work is therefore always counted.

## Modify semantics

Every modify is handled as **cancel + reinsert**: the order is removed and
re-added at its new price and quantity, losing time priority. That is exactly
how a production exchange treats a reprice or a size increase, and every modify
in the canonical workload is one of those — specifically, every modify is a
size **increase** (`new_quantity = old_quantity + 1`), and 80% of those are
**also** a one-tick reprice on top. (A pure same-price quantity *decrease*
keeps priority in production; the canonical workload contains none, so the
three reference engines — one of which has no safe in-place modify — stay in
exact agreement.) The engine implements `engine_on_modify` itself; it emits
one Trade per crossing fill and one ModifyAck — or, for a modify of an order
that is not resting, one ModifyReject.

## Measurement protocol

A full challenge for one engine, per scenario, is:

- **10 perf runs** — each times the workload (with `engine_flush()` inside the
  window) and verifies the output hash. The reported throughput is the **median**
  of the ten; report the standard deviation alongside it.
- **1 audit run** — replays the workload through a baseline engine and runs the
  random-point state audit (`docs/ANTI_CHEAT.md`). Not timed.

The result is **VALID only if all ten perf hashes pass and the audit run
passes**. Each run is a fresh process (`scripts/run_challenge.py` drives all
eleven). Anti-cheat probing happens on every run, so the perf runs are
indistinguishable from the audit run to the engine, but only the audit run
compares the probes — the measured runs are never slowed by the check.

## Correctness

Correctness is verified over the engine's **whole output stream** — every
OrderAck, Trade, CancelAck, ModifyAck, CancelReject, and ModifyReject. The
harness collects every report and stable-sorts them by `(sequence_number,
type)`, then serialises one per line. The line format depends on the report
type; the leading field is the `me_report_type_t` value:

    0  OrderAck      0,seq,side,order_id,price_ticks,quantity
    1  Trade         1,seq,price_ticks,quantity,maker_order_id,taker_order_id
    2  CancelAck     2,seq,side,order_id,price_ticks
    3  ModifyAck     3,seq,side,order_id,price_ticks,quantity
    4  CancelReject  4,seq,order_id
    5  ModifyReject  5,seq,order_id

joined by `\n` with no trailing newline. The SHA-256 of those UTF-8 bytes is
the correctness hash. `reference/correctness_hash.txt` holds a published hash
for every scenario at seed 12345 (one `<sha256>  <tag>` line per scenario);
`reference/canonical_output.txt.gz` is the exact text that hashes to the
canonical `normal` + seed 12345 entry (gzip-compressed; decompress with
`gunzip -k` or regenerate via `./harness --scenario normal --mode audit
--write-reference`). The other four scenarios' canonical-output texts are not
shipped — regenerate them locally with `--write-reference --scenario <name>`.

Sorting by `(sequence_number, type)` makes the canonical form independent of
the order in which an engine emits one message's reports — an engine that emits
a ModifyAck before the modify's trades and one that emits it after serialise
identically. Each workload message has a unique sequence number, so the sort
groups a message's reports together; the stable sort keeps the trades within a
message in match order. CancelAck omits quantity (an IOC residual is implied by
the trades and the OrderAck), and a reject carries only identity, since the
cancelled / modified order is gone.

The published hash is the **byte-identical consensus of three independent
public engines** — Liquibook [liquibook], QuantCup [quantcupPTME], and
Exchange-core [exchangecore]. On `normal` + seed 12345 all three agree on the
entire report stream — 71,851 trades among ~2.21M reports. Three unrelated
codebases agreeing makes the reference credible and free of any single engine's
bias. See `docs/PATCHES.md` for how each baseline is built — each has been
corrected for documented defects and trade-price-convention mismatches.

## References

Citation keys match those in the accompanying paper's bibliography
(`acmart.bib`); the entries below are summaries for convenience and should be
verified against that file before use.

**Price grid.**
- [secRule612_2024] U.S. Securities and Exchange Commission. *Amendments to
  Rule 612 of Regulation NMS* (Minimum Pricing Increments). Adopting release
  34-101070, September 18, 2024.
  <https://www.sec.gov/files/rules/final/2024/34-101070.pdf>

**Order placement — power-law depth profile.**
- [bouchaud2002orderbook] J.-P. Bouchaud, M. Mézard, and M. Potters.
  "Statistical properties of stock order books: empirical results and models."
  *Quantitative Finance* 2(4), 2002.
- [gould2013lob] M. D. Gould, M. A. Porter, S. Williams, M. McDonald, D. J.
  Fenn, and S. D. Howison. "Limit order books." *Quantitative Finance* 13(11),
  2013.
- [zovkofarmer2002patience] I. Zovko and J. D. Farmer. "The power of patience:
  a behavioural regularity in limit-order placement." *Quantitative Finance*
  2(5), 2002.
- [cont2010stochastic] R. Cont, S. Stoikov, and R. Talreja. "A stochastic model
  for order book dynamics." *Operations Research* 58(3), 2010.
- [munitoke2013queueing] I. Muni Toke. Queueing-model treatment of limit-order-
  book dynamics, 2013.

**Order lifecycle.**
- [secTradeToOrderVolume2022] U.S. Securities and Exchange Commission.
  Trade-to-order volume statistics (low single-digit trade-to-order ratios in
  NMS equities).
- [khomynPutnins2021] M. Khomyn and T. J. Putniņš. Order-to-trade ratios and
  market quality, 2021.
- [liYeZheng2023] S. Li, M. Ye, and M. Zheng. On the material share of
  immediate-or-cancel-like liquidity-taking instructions in U.S. equity flow,
  2023.
- [secQuoteLife2013] U.S. Securities and Exchange Commission. Quote-life study,
  2013.
- [secQuoteLifeCondFreq2025] U.S. Securities and Exchange Commission.
  Quote-life conditional-frequency data, 2025.

**Reference engines.**
- [liquibook] Liquibook. OCI / Object Computing.
- [quantcupPTME] QuantCup Price-Time Matching Engine, 2010 contest sponsored by
  Tower Research Capital.
- [exchangecore] Exchange-core open-source matching engine.
