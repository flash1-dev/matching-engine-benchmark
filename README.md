# Matching Engine Benchmark

A reproducible benchmark for limit-order-book matching engines. Published with
*"The World's Fastest Matching Engine Algorithm"* (Flash One Technologies, 2026).

The harness replays a fixed, deterministic order-flow workload through an engine
loaded as a shared library, verifies the result against a cryptographic hash,
and measures throughput — so any two engines can be compared on identical work.

## Quick start

```sh
git clone <repo-url> matching-engine-benchmark
cd matching-engine-benchmark
make                                   # builds ./harness and ./generator
scripts/build_baselines.sh liquibook   # fetch + build one baseline engine
./harness --baseline liquibook --scenario normal --mode perf
./harness --baseline liquibook --scenario normal --mode audit
```

`scripts/build_baselines.sh all` builds all three baselines (Exchange-core also
needs a JDK 11 and Maven; see *Requirements*).

To benchmark your own engine, implement the C ABI in
`api/matching_engine_api.h`, build it as a `.so`, and:

```sh
./harness --engine /path/to/your_engine.so --scenario normal --mode perf
```

See `docs/INTEGRATION.md`.

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

Three reference engines span the design space — Liquibook (tree of lists),
QuantCup (flat price-indexed array), and Exchange-core (direct-access order
book on the JVM). Each has a distinct failure mode, which is the point of the
five
scenarios: QuantCup's flat array is fastest when prices stay in a narrow band
(`static`) and collapses as they spread (`flash-crash`); Liquibook's price-keyed
multimap does the opposite; Exchange-core is volatility-flat but pays a JNI
crossing per message. All three nonetheless produce a byte-identical output
stream — every report, not just trades — and that agreement is the correctness
reference.

Throughput on the canonical workload (median of 10 trials, single matcher /
single drainer on adjacent cores, Graviton4 / Neoverse-V2,
`-O3 -march=native`):

| Engine          | static    | normal    | swing-25  | swing-40  | flash-crash |
|-----------------|----------:|----------:|----------:|----------:|------------:|
| Liquibook       | infeasible | 4.67 M/s | 4.77 M/s  | 4.76 M/s  | 4.73 M/s    |
| QuantCup        | 6.99 M/s  | 3.72 M/s  | 0.70 M/s  | 0.47 M/s  | 0.35 M/s    |
| Exchange-core   | 1.37 M/s  | 1.89 M/s  | 1.82 M/s  | 1.91 M/s  | 1.90 M/s    |
| **FlashOne**    | **30.16 M/s** | **30.68 M/s** | **31.26 M/s** | **31.18 M/s** | **31.34 M/s** |

FlashOne is the harness publisher's production engine — included as a
published reference; its `.so` is not publicly available, so the row is
reproducible only under a production license. See `discoveries.md` for
per-engine architecture notes, observations from eleven further surveyed
engines, and how to interpret each row.

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
/ 15% IOC, a GBM mid-price walk, and every report drained to a separate core.
The two are **not** like-for-like; the table records both so the difference is
visible (per-engine conditions and correctness findings are in
`discoveries.md`).

| Engine       | Harness `normal` (full report drainage) | Project's published figure |
|--------------|----------------------------------------:|---------------------------:|
| piyush       | 2.33 M/s                                | ~160 M/s                   |
| philipgreat  | 5.22 M/s                                | ~125 M/s ("8 ns/order")    |
| limitbook    | 2.34 M/s (INVALID)                      | ~30 M/s                    |
| robaho       | 3.70 M/s                                | 10–22 M/s                  |
| geseq        | infeasible (>540 s/trial)               | 12.5–21 M/s                |
| mansoor      | ≤0.03 M/s                               | >20 M/s                    |
| jxm35        | 1.86 M/s                                | 14 M/s                     |
| femto_go     | 0.006 M/s (~350 s/trial)                | >10 M/s                    |
| CppTrader    | 5.36 M/s                                | ~3.2 M/s                   |
| OrderBook-rs | 0.79 M/s (INVALID)                      | latency-focused            |
| Tzadiko      | infeasible                              | not headlined              |

Every engine that advertises a double-digit-million-per-second (or higher)
figure lands in low single digits of M/s — or does not complete — once reports
cross a thread boundary under a cancel-heavy, multi-price workload; FlashOne
sustains ~31 M/s on that same workload (above). Two engines (limitbook,
OrderBook-rs) also diverge from the byte-identical correctness consensus on
`normal`; see `discoveries.md` for the specific findings.

## How it works

- **Workload** — a deterministic, realistically-shaped equity order flow: a
  geometric-Brownian-motion mid-price, power-law order depth, and a 95% cancel /
  15% IOC / 20% modify lifecycle with occasional duplicate cancels and modifies.
  Hand-rolled distributions make it bit-identical on any platform.
  `docs/METHODOLOGY.md`.
- **Correctness** — the engine's full report output is hashed (SHA-256) and
  checked against `reference/correctness_hash.txt`, which ships a hash for
  every scenario at seed 12345. The canonical entry — `normal` + seed 12345 —
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
tests/                  a SHA-256 self-test and the anti-cheat cheat adapter
discoveries.md          observations the harness produced against the eleven surveyed engines
```

## Requirements

- Linux, GCC 14+ or Clang 16+ (C++20), CMake 3.16+, Boost headers.
- `scripts/build_baselines.sh` additionally needs `git`; for the Exchange-core
  baseline it needs a JDK 11 and Maven.
- Python 3.8+ for the wrapper scripts.

## License

MIT — see `LICENSE`. The three baseline engines are fetched from their own
upstream repositories under their own licenses; `docs/PATCHES.md` records every
modification made to build them.
