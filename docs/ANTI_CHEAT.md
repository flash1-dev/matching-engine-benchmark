# Anti-cheat

The harness produces a number engines will be compared on, so it must resist
engines tuned to the benchmark rather than to matching. It does this without
slowing the engine down: anti-cheat and measurement run as **separate passes**.

## Two run modes

`./harness` runs in one of two modes (`--mode`):

- **perf** — the measured pass. It times the workload and reports throughput,
  and it verifies the engine's output against the published hash. It does *not*
  run the state audit.
- **audit** — the anti-cheat pass. It replays the workload through a trusted
  baseline engine and compares order-book state at random points. It also
  verifies the output hash. It is *not* timed.

A full challenge for one engine is **10 perf runs + 1 audit run** per scenario
(`scripts/run_challenge.py`). The reported throughput is the median of the perf
runs; the result is **VALID only if every perf run's hash passes and the audit
run passes**. Splitting the passes means the measured runs carry no audit
*time* charged against throughput — the audit's probe wall-time is excluded
from the timed window via an `excluded_ns` accumulator — while the audit
pass, freed from the clock, validates the book.

The split only works if an engine cannot tell which pass it is in. It cannot:
**every run, perf or audit, probes `engine_query_*` at the same random points**
(see *Random-point state audit*). A perf run discards the answers; the audit
run compares them. Because the engine sees an identical sequence of calls
either way, it cannot be a real engine only when audited.

## Correctness hash

Every run — perf and audit — hashes the engine's full report stream (SHA-256)
and compares it to the published reference (`docs/METHODOLOGY.md`). An engine
that emits wrong or invented reports fails here. The reference is the consensus of
three independent public engines, so it cannot be skewed toward any one design.

A run with no matching reference entry — a custom seed/scenario not in
`reference/correctness_hash.txt`, or a run with `--write-reference` —
reports `Status: NO REFERENCE` or `Status: REFERENCE WRITTEN`, both of
which yield `Verdict: INVALID`. The harness only marks VALID on an
affirmative `Status: PASS`.

`tests/cheat_adapter.cpp` is the executable demonstration: a "fake engine"
that replays the canonical text verbatim. It passes the perf hash trivially
(`Verdict: VALID` in perf-only) but fails the state audit (`Verdict:
INVALID` under audit) because it maintains no real book — making the
random-point probes impossible to answer correctly.

## Pipeline barrier

The engine emits its own report stream and may match on any thread(s) it likes.
To stop an engine from looking fast by returning before its work is done, the
harness calls `engine_flush()` after the last message and **inside the timed
window**. `engine_flush()` must not return until every message has been fully
matched and every report emitted. Any work an engine defers to a background
thread is therefore still counted — deferring buys nothing.

## Pre-build is translation-only

The optional `engine_prebuild` hook (`api/matching_engine_api.h`) runs once per
message *before* the timed window, so an engine can marshal each message into
its native order representation off the clock — modelling the gateway parse a
real venue does outside the matcher. That hook is the one place an engine could
cheat by doing **matcher** work early: allocating the resting node, inserting
into the book, or pre-matching, then replaying a cached result in the timed
call. The contract forbids it — prebuild may only translate and pre-size static
capacity — and the harness enforces the visible half: immediately after the
prebuild pass and before it starts the clock, it asserts the book is empty
(`engine_query_best_bid() == INT64_MIN` and `engine_query_best_ask() ==
INT64_MAX`). An engine that rested orders during prebuild is gated INVALID with
an `Anti-cheat: pre-start book not empty by the API sentinels` line, regardless of whether the output
hash matches. `tests/prebuild_insert_cheat.cpp` is the executable
demonstration: it inserts during prebuild and is caught here in a plain perf
run.

The book-empty assert catches pre-insertion. It does not catch a *shadow*
pre-matcher — one that matches into a private structure (leaving the queryable
book empty) and replays cached results in the timed call. A second guard covers
that: the harness times the prebuild pass and compares it to the timed run.
Translation is a small fraction of matching, so an honest prebuild runs well
under the timed window (Liquibook 0.02x, QuantCup 0.06x, FlashOne 0.53x on
`normal`); a shadow pre-matcher front-loads the match, so its prebuild rivals or
exceeds the run. Above 2x the harness prints a loud `Anti-cheat: pre-build ran
Nx the timed window` flag; above 4x — a level no honest translation reaches — it
gates the run INVALID. `tests/shadow_prematch_cheat.cpp` is the demonstration:
it passes the book-empty assert and is caught by the time bound. (The bound
bites hardest exactly when a cheat succeeds: beating an honest engine needs a
small timed window, which makes the prebuild/timed ratio large.)

Together the two guards close pre-insertion and pre-matching. What remains is
order-independent work an engine could hide in its own memory — pre-allocating a
node per order without resting it leaves the book empty *and* costs about as
little as translation, so neither guard sees it. That residue is why the
contract is also a rule engines are expected to honour; the fully robust
alternative is to retire the hook and translate inside the timed loop (a small,
uniform cost paid by every engine). The two guards are the cheaper option that
keeps the matcher-only measurement scope.

## Random-point state audit

A hardcoded engine could replay the published trades without maintaining an
order book at all. To catch that, the harness records the engine's answers to
`engine_query_best_bid`, `engine_query_best_ask`, and `engine_query_depth_at`
at 64 **unpredictable** workload indices (192 state checks in total — 3
queries per index), chosen from a random verification seed that differs every
run. The audit run then replays the same workload through a trusted public
baseline engine and compares every recorded answer; the result JSON's
`audit.checks` field is the total state-check count (192 when the audit runs
to completion).

If two engines produce the same trades they must hold the same resting book at
every step, so a genuine engine matches the baseline exactly. An engine that
did not actually maintain the book cannot, and the run is INVALID. Because the
probe indices come from a per-run random seed they cannot be precomputed; the
seed used is recorded in the result JSON for reproducibility.

Every run probes these points — a perf run just discards the comparison — so an
engine cannot distinguish a measured run from an audited one. Under batched
delivery the harness ends each batch exactly at these audit-probe indices, so
the book is probed at the same per-message points either way (see
`docs/METHODOLOGY.md`, *Batch delivery*).

## Held-out seeds: the strongest test

The shipped reference covers every scenario at the canonical seed 23 (its
`normal` entry being the one whose full report stream is also published,
`reference/canonical_output.txt.gz`, so anyone can regenerate and verify the
hash). That convenience has a limit a determined cheat
could exploit: with the canonical output in hand, an engine could *replay* it to
reproduce the hash, and *reconstruct* the resting book from those same canonical
trades to answer the state-audit probes — clearing both checks without doing any
real matching.

The defense is a **held-out seed** the engine's author has no reference for.
Generate a private reference with a trusted baseline:

```sh
./harness --baseline liquibook --scenario normal --seed <private-seed> --write-reference
```

then challenge the engine on it:

```sh
./harness --engine ./their_engine.so --scenario normal --seed <private-seed> --mode perf
./harness --engine ./their_engine.so --scenario normal --seed <private-seed> --mode audit
```

With no public canonical to replay, the only way to produce the correct report
stream is to actually match the orders, and the only way to answer the audit is
to actually maintain the book. The shipped seed-23 reference is for convenient
self-checking and head-to-head reproduction; an adversarial evaluation should
use a private held-out seed (any 32-bit value works — the workload shape is
unchanged, only its realisation differs).

## The engine may use threads

There is no single-thread rule. A production engine that matches on one core
and publishes reports from another is running its real architecture, and the
harness measures it that way. The process thread count is recorded in the
result JSON (`threads`) as an informational figure; it never affects the
verdict.

## Fresh process, regenerated workload

Each invocation is a fresh process and the workload is regenerated from its
seed, so no state can be carried across runs.

## Platform fingerprint

Every result JSON embeds a host fingerprint — CPU model and a `/proc/cpuinfo`
digest, core count, total memory, kernel, compiler, and the AWS instance type
when running on EC2 — so a throughput number is always tied to the hardware it
was measured on.

## Defensive execution

Engine calls run under `SIGSEGV` / `SIGABRT` / `SIGBUS` / `SIGFPE` handlers and
an `alarm()` watchdog. An engine that crashes or hangs is reported as failed
rather than taking the harness down with it.

## What the harness does not police

The harness measures the matching algorithm in isolation. It does not sandbox
the engine, inspect its binary, or attempt to detect every possible abuse — it
checks that an engine does the right amount of real book work and produces the
consensus-correct output. Results remain a claim made by whoever ran them; the
fingerprint and the recorded verification seed are what let a third party
re-run and confirm.
