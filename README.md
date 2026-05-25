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
| Liquibook       | infeasible | 4.70 M/s | 4.75 M/s  | 4.73 M/s  | 4.75 M/s    |
| QuantCup        | 6.99 M/s  | 3.72 M/s  | 0.70 M/s  | 0.47 M/s  | 0.35 M/s    |
| Exchange-core   | 1.23 M/s  | 1.69 M/s  | 1.72 M/s  | 1.72 M/s  | 1.76 M/s    |
| **FlashOne**    | **29.43 M/s** | **30.15 M/s** | **30.23 M/s** | **29.96 M/s** | **30.22 M/s** |

FlashOne is the harness publisher's production engine — included as a
published reference; its `.so` is not publicly available, so the row is
reproducible only under a production license. See `discoveries.md` for
per-engine architecture notes, observations from eight further surveyed
engines, and how to interpret each row.

Measure on your own platform:

```sh
scripts/build_baselines.sh all
scripts/run_challenge.py --compare liquibook quantcup exchange_core --all-scenarios
```

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
additional_references/  eight worked adapter examples for third-party engines (C++, Rust, Go)
patches/                source patches applied to baseline engines (QuantCup)
reference/              the published canonical report output and its hash
scripts/                build_baselines.sh, run_challenge.py, compare_results.py
docs/                   METHODOLOGY, INTEGRATION, ANTI_CHEAT, PATCHES
tests/                  a SHA-256 self-test and the anti-cheat cheat adapter
discoveries.md          observations the harness produced against the eight surveyed engines
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
