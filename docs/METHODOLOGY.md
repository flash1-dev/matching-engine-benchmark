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
over the run is the scenario's target swing. This is the paper benchmark's
construction, ported verbatim onto the portable RNG. GBM is a standard
construction and is not attributed to a single source; for the embedding of
GBM into a stochastic limit-order-book see [cont2010stochastic].

### Order placement

Every new order is placed **passively on its own side of the mid**: its
distance from the mid is drawn from a power-law depth profile (exponent
alpha = 2.23) with a Gaussian "hump" about 8 ticks out, reproducing the
near-touch liquidity concentration of real books. Quantity is uniform on
[1, 100]. Buy flow is priced along the first half of the mid path and sell
flow along the second (arrival order is then fully shuffled, so the wire
stream interleaves the sides) — the paper benchmark's construction. Nothing
is priced through its own mid at placement; crossing happens where the
shuffle interleaves orders priced off different parts of the walk, and where
a modify's one-tick reprice meets its counterpart at the mid. The power-law
decay of update intensity per depth level (`#updates(ℓ) ∝ ℓ^−β`, `β > 1`)
and the roughly exponential decay of resting queue length with distance from
the touch are the empirical regularities documented across
[bouchaud2002orderbook; gould2013lob; zovkofarmer2002patience;
cont2010stochastic; munitoke2013queueing].

### The standing book

How deep a resting book a run carries is a property of the price path's
**realisation**, not of the model: when a realisation's two halves separate,
part of the early side's resting tail ends up out of the late side's reach
for the remainder of the session and stands in the book until cancelled — a
path that straddles its start instead re-sweeps everything it leaves behind.
The **canonical seed (23)** is a calibration choice: it is published because
each scenario's realisation carries a standing book representative of the
regime the paper's own benchmark runs exercised, so an engine pays for its
resting-order data structures (per-order nodes, level indexes, price trees)
at realistic occupancy, not just for its message dispatch. Simulated against
a reference matcher, the canonical workloads (seed 23) carry:

| Scenario | Standing orders (avg / peak) | Standing price levels (avg / peak) | Fills |
|---|---:|---:|---:|
| `static` | ~20,900 / ~41,800 | ~60 / ~90 | ~800 |
| `normal` | ~2,700 / ~5,500 | ~290 / ~330 | ~62,000 |
| `swing-25` | ~1,300 / ~2,600 | ~830 / ~1,400 | ~67,000 |
| `swing-40` | ~700 / ~1,400 | ~560 / ~1,000 | ~69,000 |
| `flash-crash` | ~240 / ~500 | ~210 / ~400 | ~70,000 |

A fixed mid lets the book pile deepest (`static` — nothing ever sweeps it);
the wider a scenario's swing, the more of the standing tail the shuffled
crossing flow reaches, so depth thins and the surviving book spreads across
more price levels as volatility rises. The profile mirrors the paper
benchmark's own realisations scenario for scenario.

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

### Order-identifier tracking

On every cancel, modify, and fill, an engine must map the harness's client
order id back to its own internal order handle.
So that this step is measured apples-to-apples against engines that allocate
their order tables statically — QuantCup, for instance, indexes a flat
price array directly — the reference adapters perform that translation with
flat-array direct indexing rather than a hash map (no adapter-side locks, no
per-message allocation; each adapter README documents its mapping). Flat
indexing is also the
more faithful model of real order-entry protocols, where a session assigns
increasing, dense order identifiers — the sequential UserRefNum discipline of
Nasdaq OUCH 5.0 — that a direct index serves exactly.

### The five scenarios

| Scenario | Annualised vol | Target swing | Character |
|---|---:|---:|---|
| `static` | 0.00 | 0% | Fixed mid; the deepest standing book. Isolates data-structure occupancy cost; trades sparsely (one-tick modify reprices meeting at the mid). |
| `normal` | 0.15 | 2% | Routine intraday session. **Canonical** — the byte-identical consensus is anchored on `normal` at the canonical seed (23). |
| `swing-25` | 0.50 | 25% | High-volatility day. |
| `swing-40` | 0.50 | 40% | Stressed market. |
| `flash-crash` | 0.50 | 60% | Flash-crash dislocation, after documented intraday events such as the May 6, 2010 U.S. equity flash crash. |

The scenarios sweep two stress axes at once: resting-set occupancy is
greatest where the path moves least (`static`, then `normal`), and the price
range the book's level structures must span is greatest where it moves most
(`flash-crash`'s walk covers tens of thousands of ticks). Every scenario
produces trades; none is insert-only. The four moving scenarios each fill in
the tens of thousands; `static` fills sparsely by design — its matching
pressure is the standing book itself.

## Determinism

The workload must reproduce bit-for-bit so the published correctness hash means
the same thing on every machine. Only `std::mt19937` is bit-portable across C++
standard libraries — `std::normal_distribution`, `std::discrete_distribution`,
and the distributions inside `std::shuffle` are not — so the generator
hand-rolls every distribution (uniform, normal via Box–Muller, exponential, the
depth-profile CDF, and the order-of-arrival shuffle) directly on `std::mt19937`.
That makes the RNG word stream and the distribution algorithms identical
everywhere.

One residual dependency remains: those hand-rolled distributions call libm
transcendentals (`std::cos` / `exp` / `log` / `pow`), which are not standardized
to the last bit across libm implementations (glibc, musl, Apple, MSVC). The
workload reproduces bit-for-bit across mainstream glibc builds — the shipped
reference was generated on glibc/aarch64 — and the **shipped reference hash is
the canonical artifact**. On an exotic libm the stream could in principle
differ; if so, regenerate the reference locally with a trusted baseline
(`--write-reference`).

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

`orders_<scenario>_s<seed>_n<count>.bin` (keyed on seed and count so a held-out
seed never reuses a cached workload): a 16-byte header `[u64 magic][u32 version][u32 count]`
then `count` fixed 40-byte records:

    [u8 type][u8 side][u8 ioc][u8 pad][u32 quantity]
    [u64 sequence_number][u64 order_id][i64 price_ticks][i64 reserved]

`type` is 0 = NEW, 1 = CANCEL, 2 = MODIFY. This file is a harness-internal
cache; it is not part of the engine ABI.

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
keeps priority in production; the canonical workload contains none, so
every conforming engine stays in exact agreement — including one founding engine
that has no safe in-place modify.) The engine implements `engine_on_modify` itself; it emits
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

Across the five scenarios, `run_challenge.py` reports each engine's
**worst-case** throughput — the lowest of its five scenario medians, and the
scenario that produces it — as the engine's definitional result. This is the
basis for the comparison tables in the README: an engine is
rated by the regime it handles worst, not its best, because the matcher must
absorb the burst in whatever regime the market happens to be in.

### Order-identifier resolution

Every cancel, modify, and fill names an order by its client order id, so on
each one the engine — or its adapter — must resolve that id to the engine's own
internal order, a lookup on the hot path. Where that resolution lives differs
by engine: one whose public API takes the client id directly (FlashOne,
Exchange-core) resolves it internally, while one that returns its own handle on
insert (Liquibook, QuantCup) has the adapter hold the id→handle map (each
adapter README documents its mapping). To keep the lookup apples-to-apples —
and not penalise an engine that allocates its order table statically, the way
QuantCup indexes a flat price array directly — the adapters do their half of
the translation with flat-array direct indexing (a vector indexed by order id;
no lock, no per-message allocation) rather than a general hash map.

This also mirrors real order-entry protocols: a session assigns increasing,
dense order identifiers — the sequential UserRefNum discipline of Nasdaq
OUCH 5.0 — and the harness's ids follow it exactly, so a direct index by order
id is both the fastest structure and the one a production gateway would use. An
id outside the live range resolves to "not resting" (a CancelReject or
ModifyReject), so the scheme never trades correctness for speed.

## Batch delivery (ABI-crossing-taxed engines)

By default the harness drives an engine **one message at a time** — one
`engine_on_new_order` / `engine_on_cancel` / `engine_on_modify` call per workload
message (`api/matching_engine_api.h`). For a native engine (C, C++, Rust) that
call is a direct branch into the matcher and costs almost nothing. For an engine
whose matcher sits behind a **language-runtime boundary**, it is not: every
inbound call crosses that boundary, and for some runtimes the crossing dwarfs the
matching work. A Go matcher reached through cgo pays the runtime-entry cost
(`needm`) on every call — *tens of microseconds* when the calling thread is
foreign to the Go runtime, because the runtime re-derives that thread's stack
bounds from `/proc/self/maps` on each entry — and a Java matcher reached through
JNI pays a method-call trampoline per message. Driven one at a time, such an
engine's throughput measures its **ABI boundary, not its matcher**.

To measure those engines on their matchers, an engine MAY export the optional
`engine_on_batch(msgs, n)` entry point. The harness then delivers the workload as
runs of messages — one call per run — and the engine loops over the run
internally, paying the boundary crossing **once per run instead of once per
message**. The matching is unchanged: each message is processed in array order,
exactly as if delivered alone, with no cross-message lookahead.

**The audit still works.** The harness does not hand over one giant batch; it
ends each batch exactly at the next random state-audit probe index, queries the
book there, then continues — so the book is inspected at the same unpredictable
per-message points as under one-at-a-time delivery. The full report-stream hash
and the random-point state audit (`docs/ANTI_CHEAT.md`) are therefore byte-for-
byte identical to per-message delivery, and a batched run is gated VALID on
exactly the same basis. Two mechanisms keep an engine honest without trusting it:
the report stream must reproduce the consensus hash (any cross-message lookahead
or netting changes the emitted reports and fails it), and the book is probed at
the same points it would be one message at a time.

**Recommendation.** An engine whose runtime taxes each ABI crossing — **Go
(cgo), Java (JNI)**, and any other managed or foreign runtime reached across the
C ABI — should implement `engine_on_batch`; without it the harness measures the
crossing, not the matcher, and the engine's figure understates it by the boundary
cost (for cgo, by orders of magnitude). A native engine (C/C++/Rust) needs it
only for a small gain: batched delivery also amortizes the harness's own
per-message dispatch — one indirect call through the `dlopen`'d symbol, ~2 ns,
which registers only for an engine fast enough for that to matter. The comparison
tables in the README therefore report the batched figure for
the engines it moves materially; for every other engine the two coincide within
measurement noise.

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
for every scenario at the canonical seed 23 (one `<sha256>  <tag>` line per scenario);
`reference/canonical_output.txt.gz` is the exact text that hashes to the
canonical `normal` + seed 23 entry (gzip-compressed; decompress with
`gunzip -k` or regenerate via `./harness --baseline liquibook --scenario normal
--write-reference`). The other four scenarios' canonical-output texts are not
shipped — regenerate them locally with `./harness --baseline liquibook
--scenario <name> --write-reference`.

Sorting by `(sequence_number, type)` makes the canonical form independent of
the order in which an engine emits one message's reports — an engine that emits
a ModifyAck before the modify's trades and one that emits it after serialise
identically. Each workload message has a unique sequence number, so the sort
groups a message's reports together; the stable sort keeps the trades within a
message in match order. CancelAck omits quantity (an IOC residual is implied by
the trades and the OrderAck), and a reject carries only identity, since the
cancelled / modified order is gone.

The published hash is the **byte-identical consensus** — the report stream
every conforming engine reproduces. It was first established from three
independent public engines (Liquibook [liquibook], QuantCup [quantcupPTME], and
Exchange-core [exchangecore]): on `normal` at seed 23 all three agree on the
entire stream — 62,474 trades among ~2.2M reports — and the wider conforming
field has since reproduced it byte-for-byte across 100 seeds. Unrelated
codebases agreeing makes the reference credible and free of any single engine's
bias. See `docs/PATCHES.md` for how each baseline is built — QuantCup carries a source
patch (a trade-price-convention fix, plus build-enabling and price-domain changes);
Liquibook and Exchange-core are built from unmodified source.

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
- [quantcupPTME] QuantCup Price-Time Matching Engine, 2011 contest sponsored by
  Tower Research Capital.
- [exchangecore] Exchange-core open-source matching engine.
