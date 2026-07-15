# Integrating an engine

An engine under test is a shared library (`.so`) that exports the C ABI in
`api/matching_engine_api.h`. The harness loads it with `dlopen` and, by
default, drives it one message at a time (see *Batch delivery* in
`docs/METHODOLOGY.md`). This document is the how-to; the three adapters in
`adapters/` are the minimal worked examples, and `additional_references/`
contains 40 worked adapters (the permissively-licensed subset of the ~134
built across the 20+-language survey; the rest are held data-only), several of
which patch the upstream source at build time and may be the closest
template depending on the engine's shape.

## The contract

Export these symbols with C linkage:

| Symbol | Role |
|---|---|
| `engine_init(uint64_t seed, const me_transport_t*, void* report_sink)` | One-time setup. Keep the transport + sink — the engine pushes reports into them. |
| `engine_shutdown(void)` | One-time teardown. |
| `engine_on_new_order(const new_order_t*)` | Process a new order; emit an OrderAck, a Trade per fill, and an IOC-residual CancelAck if applicable. |
| `engine_on_cancel(const cancel_t*)` | Process a cancel; emit a CancelAck, or a CancelReject if no such order is resting. |
| `engine_on_modify(const modify_t*)` | Process a modify (cancel + reinsert); emit a Trade per crossing fill and a ModifyAck, or a ModifyReject if no such order is resting. |
| `engine_flush(void)` | Pipeline barrier — see *Pipeline barrier* below. |
| `engine_query_best_bid(void)` | Highest bid price in ticks, or `INT64_MIN`. |
| `engine_query_best_ask(void)` | Lowest ask price in ticks, or `INT64_MAX`. |
| `engine_query_depth_at(int64_t price, uint8_t side)` | Aggregated resting quantity at one price level. |

`engine_get_transport` is optional — see *Custom transport* below. So are
`engine_on_batch` and `engine_prebuild` (see `api/matching_engine_api.h`).

### The engine emits its own reports

The hot-path calls return nothing. For each message the engine produces the
reports it generates and pushes each into the report transport:

```c
me_report_t r = {0};
r.type = ME_TRADE;          /* any of the six me_report_type_t values */
r.sequence_number = ...;    /* the aggressive order's sequence_number          */
/* ... fill the fields ... */
while (!transport->push(report_sink, &r)) { /* spin: queue full */ }
```

`transport` and `report_sink` are the two arguments handed to `engine_init`.
The harness drains the stream on a separate core; that hand-off is the
matcher-to-publisher cost a real exchange pays, and it is inside the measured
window.

The report types and when to emit them:

- **OrderAck** — one per accepted new order.
- **Trade** — one per fill. `price_ticks` is the **maker's** (resting order's)
  price; `sequence_number` is the **aggressive** (incoming) order's;
  `maker_order_id` / `taker_order_id` are the two orders. Emit fills in match
  order.
- **CancelAck** — one per successful cancel, and one per IOC residual (the
  unfilled remainder of an IOC order — match what you can, drop the rest).
- **ModifyAck** — one per successful modify.
- **CancelReject** — one per cancel of an order that is not resting (already
  filled, already cancelled, or never seen).
- **ModifyReject** — one per modify of an order that is not resting.

### Rules

- **Emit a correct report stream.** Correctness is checked over the whole
  report stream — every report type (`docs/METHODOLOGY.md`).
- **Threads are allowed.** Match and report on whatever thread(s) you like —
  whatever reflects how the engine runs in production. There is no
  single-thread restriction.
- Prices are signed integer ticks ($0.005 each). Order ids fit in 32 bits for
  the standard workload; use 64-bit types anyway.
- `engine_query_*` must reflect the live order book — they are called at
  unpredictable points and compared against a baseline engine. For an
  asynchronous engine, a query must observe every message delivered before it.

### Pipeline barrier

The harness calls `engine_flush()` once, after the last message and before it
stops the clock. It must not return until every message delivered so far has
been fully matched and every resulting report has been pushed into the
transport. A fully synchronous engine implements it as a no-op; an engine that
matches or reports asynchronously must drain its own pipeline here. Because
`engine_flush()` is inside the timed window, deferred work is always counted.

### Modify

The harness models modify as cancel + reinsert (`docs/METHODOLOGY.md`):
`engine_on_modify` removes the order and re-adds it at the new price/quantity,
losing queue priority. Emit a Trade per crossing fill and exactly one
ModifyAck — or, if no such order is resting, a ModifyReject. Every modify in
the canonical workload is a quantity increase (`new_quantity = old_quantity +
1`); 80% of those are also a one-tick reprice. All reference adapters do
exactly cancel + reinsert.

## Building an adapter

```sh
g++ -std=c++20 -O3 -march=native -fPIC -shared -I api \
    my_adapter.cpp my_engine_sources... -o my_engine_adapter.so
```

The only header you need is `api/matching_engine_api.h`. The adapter is the
glue between that ABI and your engine; keep engine-specific bookkeeping in the
adapter so the engine itself stays unmodified where possible. The reference
build recipe is `-O3 -march=native -fPIC`: `-march=native` mirrors how a
production engine would be built for the host. The measured effect on
matching throughput is small — typically within a few percent of a portable
`-O3` build — because matchers are memory-latency-bound, but the convention
matches production tooling. If the engine's source genuinely needs a newer
C++ standard (e.g. jxm35 needs `-std=c++23` for `std::expected`), match the
engine's requirement and note the deviation in the adapter's build.sh. For
non-C++ adapters use the equivalent host-CPU flag (`RUSTFLAGS="-C
target-cpu=native"` for Rust cdylibs; Go's `c-shared` build mode does not
need an explicit flag).

### Non-C++ engines

The ABI is C, so any language with a stable C FFI works as the engine
implementation. Two patterns are demonstrated in `additional_references/`:

- **Rust (`orderbookrs_adapter`)** — a single `cdylib` exports the harness
  `engine_*` symbols directly via `#[no_mangle] extern "C"`. No C++ shim.
  Built with `cargo build --release`.
- **Go (`geseq_adapter`)** — a `package main` with `//export` directives,
  built with `go build -buildmode=c-shared` to produce a `.so` with plain C
  symbols. cgo handles the Go ↔ C boundary.

A JVM engine (e.g. `adapters/exchange_core_adapter.cpp` +
`adapters/HarnessExchangeCore.java`) embeds a JVM via JNI and calls a thin
Java helper class.

An engine reached across a managed or foreign runtime (Go/cgo, Java/JNI) should
implement `engine_on_batch` or its figure measures the ABI boundary, not the
matcher — the `geseq`, `femto_go`, and Exchange-core reference adapters do this.

### Patching upstream source

Several reference adapters patch the upstream engine source at build time
because the upstream is missing something the harness needs:

- `jxm35_adapter/build.sh` injects a per-fill `notify_trade` hook the engine
  declares but never calls.
- `tzadiko_adapter/build.sh` replaces a Windows-only `localtime_s` call,
  drops a per-match allocation hint, fixes a self-deadlock in the
  engine's FillAndKill tail-cancel (the public `CancelOrder` re-locked a
  mutex `AddOrder` already held; the patch switches the two sites to the
  engine's own already-locked `CancelOrderInternal`), and removes a
  lost-wakeup deadlock in the engine's prune-thread teardown (the
  destructor's condition-variable notify could race ahead of the waiter
  and hang `join()`; the patch polls an atomic shutdown flag instead).

Convention: apply the patch with `sed` or a Python `str.replace`, and make
it idempotent — `git reset --hard <pinned-sha>` first so a rerun starts
clean. Document each patch in the adapter's README so a reader can audit
what was changed.

## Running

```sh
./harness --engine ./my_engine_adapter.so --scenario normal --mode perf
./harness --engine ./my_engine_adapter.so --scenario normal --mode audit
```

A `perf` run times the workload and verifies the output hash; an `audit` run
runs the anti-cheat state audit (`docs/ANTI_CHEAT.md`). A full challenge is 10
perf runs + 1 audit run — `scripts/run_challenge.py` drives all eleven, reports
the median throughput, and prints the overall verdict. By default it runs all
five scenarios and reports your engine's **worst-case** throughput (the lowest of
the five, with the scenario that produces it) as its definitional result;
`--scenario` narrows to one. `--baseline <name>` is shorthand for
`--engine ./<name>_adapter.so`.

## Custom transport

By default the harness carries the report stream over its own
single-producer / single-consumer queue and hands it to the engine through
`engine_init`. An engine that wants its own inter-thread queue measured
instead may export:

    const me_transport_t* engine_get_transport(void);

returning a vtable of `create` / `push` / `drain` / `flush` / `destroy`
(`api/matching_engine_api.h`). The harness calls `create()` and passes the
handle back through `engine_init`. An engine whose matcher already emits into
its own queue may implement only `create`/`drain`/`flush`/`destroy` and leave
`push` unused. If the symbol is absent, the harness default is used. The
transport affects only how reports are carried between threads — never
correctness.

## Validating your adapter

Build a baseline first and confirm the harness reports `Verdict: VALID`:

```sh
scripts/build_baselines.sh liquibook
./harness --baseline liquibook --scenario normal --mode perf
./harness --baseline liquibook --scenario normal --mode audit
```

Then plug in your engine. A correct engine reproduces the published hash and
passes the state audit. If the hash differs, diff your output against
`reference/canonical_output.txt.gz` (decompress first with `gunzip -k`, or
regenerate via `./harness --baseline liquibook --scenario normal --write-reference`)
to find the first divergent report (the line format is in
`docs/METHODOLOGY.md`).

The harness can export any engine's canonical report stream after the timed run
without modifying the protected consensus artifacts:

```sh
./harness --engine ./my_engine_adapter.so --scenario normal --mode perf \
  --write-canonical-output /tmp/my_engine_normal.txt

scripts/explain_divergence.py \
  reference/canonical_output.txt.gz \
  /tmp/my_engine_normal.txt \
  --json-output /tmp/my_engine_divergence.json
```

`explain_divergence.py` streams reports grouped by originating sequence,
reports the first differing group, and exits `1` on divergence. The optional
versioned JSON artifact contains the reference and candidate reports at that
sequence for CI or downstream failure-reduction tools. Export happens after the
timed matcher window and neither operation changes the correctness oracle.

Run the pre-run conformance gate before benchmarking:
`scripts/conformance_check.py ./<engine>_adapter.so` (see
[`docs/CONFORMANCE.md`](CONFORMANCE.md)).
