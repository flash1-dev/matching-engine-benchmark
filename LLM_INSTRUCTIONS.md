# LLM_INSTRUCTIONS.md — connect and run a matching engine

> **Audience: an AI coding agent.** A user has pointed you at this repository
> and asked something like *"connect my matching engine to this harness and run
> it."* This file is the complete, ordered procedure. Follow it top to bottom.
> Every command assumes the current directory is the repository root
> (the folder that contains this file).
>
> Companion docs, if you need depth: `docs/INTEGRATION.md` (prose version of the
> contract), `docs/METHODOLOGY.md` (workload), `docs/ANTI_CHEAT.md` (what the
> verdict checks). The authoritative ABI is `api/matching_engine_api.h`.

---

## 0. What you are doing

This harness benchmarks a single matching engine. The engine is a **shared
library** (`.so`) that exports a fixed C ABI; the harness `dlopen`s it, replays
~2.0M order messages through it, has the engine emit its own report stream,
verifies that output against a published SHA-256 hash, runs an anti-cheat audit,
and prints `Verdict: VALID` or `INVALID`.

Your job: write an **adapter** — a small C++ file that implements the ABI by
calling into the user's engine — build it into a `.so`, and run the harness.

You do **not** modify the harness. You only add an adapter.

---

## 1. Collect this information about the user's engine

If you do not already know all of the following, inspect the engine's source
and headers, and ask the user for anything still unclear:

1. **Location** of the engine's source code or prebuilt library, and its
   **language** (C/C++, or JVM, or other).
2. How to **create an empty order book** for one symbol.
3. How to **submit a limit order** (buy/sell, price, quantity, an id you choose).
4. How to **submit an immediate-or-cancel (IOC) order**, or whether you must
   submit a normal order and then cancel the unfilled remainder yourself.
5. How to **cancel** a resting order by id.
6. How the engine **reports fills** — a return value, an event list, or a
   callback. (A callback is the most common; see the template note in §4.)
7. How to read **best bid price, best ask price, and total quantity at a
   price level**.

If the user's engine is **not C/C++**, see §4.4.

---

## 2. Build the harness and confirm it works (do this first)

```sh
make
```

Expected: this produces `./harness` and `./generator`. If `make` fails, the
build environment is missing a prerequisite — you need GCC 14+ or Clang 16+
(C++20), CMake 3.16+, and Boost headers. Fix that before continuing.

Now confirm the harness itself is healthy by running a known-good baseline:

```sh
scripts/build_baselines.sh liquibook
./harness --baseline liquibook --scenario normal --mode perf
./harness --baseline liquibook --scenario normal --mode audit
```

Expected: the perf run ends in `Status: PASS` and `Verdict: VALID`; the audit
run ends in `State audit: PASS` and `Verdict: VALID`. If those pass, the
harness works and any later failure is in the adapter, not the harness. If this
baseline does **not** pass, stop and fix the environment first — do not blame
the user's engine.

---

## 3. The integration contract

The adapter is a `.so` exporting these symbols with **C linkage**. Full
declarations and struct layouts are in `api/matching_engine_api.h` — read it.

| Function | What it must do |
|---|---|
| `void engine_init(uint64_t seed, const me_transport_t* t, void* sink)` | Create an empty order book. Save `t` and `sink` — you push reports into them. |
| `void engine_shutdown(void)` | Free everything. |
| `void engine_on_new_order(const new_order_t* o)` | Submit a new order; emit its reports (see below). |
| `void engine_on_cancel(const cancel_t* c)` | Cancel `c->order_id`; emit a CancelAck, or a CancelReject if no such order is resting. |
| `void engine_on_modify(const modify_t* m)` | Modify = cancel + reinsert; emit crossing Trades + a ModifyAck, or a ModifyReject if no such order is resting. |
| `void engine_flush(void)` | Block until all matching is done and all reports emitted (§4.3). |
| `int64_t engine_query_best_bid(void)` | Highest bid price in ticks, or `INT64_MIN`. |
| `int64_t engine_query_best_ask(void)` | Lowest ask price in ticks, or `INT64_MAX`. |
| `uint64_t engine_query_depth_at(int64_t price, uint8_t side)` | Total resting quantity at that price level (`side` 0=buy, 1=sell); `0` if empty. |

### The engine emits its own reports

The hot-path calls return nothing. The engine produces a six-type report
stream and pushes each report into the transport it was given in `engine_init`:

```c
me_report_t r = {0};
r.type = ME_TRADE;
r.sequence_number = o->sequence_number;   /* the aggressive order's seq */
/* fill the other fields */
while (!g_transport->push(g_sink, &r)) { /* spin: queue momentarily full */ }
```

- **OrderAck** (`ME_ORDER_ACK`) — one per accepted new order.
- **Trade** (`ME_TRADE`) — one per fill. `price_ticks` is the **maker's**
  resting price; `sequence_number` is the **aggressor's**; set
  `maker_order_id` and `taker_order_id`. Push fills in match order.
- **CancelAck** (`ME_CANCEL_ACK`) — one per successful cancel, and one per IOC
  residual (drop the unfilled remainder, do not rest it).
- **ModifyAck** (`ME_MODIFY_ACK`) — one per successful modify.
- **CancelReject** (`ME_CANCEL_REJECT`) — one per cancel of an order that is
  not resting (already filled, already cancelled, or never seen).
- **ModifyReject** (`ME_MODIFY_REJECT`) — one per modify of an order that is
  not resting.

### Rules that make a run VALID (violating any one makes it INVALID)

1. **Correct reports.** The full report stream must reproduce the published
   hash; for trades that means the maker's price, the aggressor's seq, and
   dropped IOC residuals.
2. **`engine_query_*` must reflect the real, live book** — the anti-cheat audit
   compares them against a baseline engine at random points.
3. **`engine_flush()` must drain the pipeline** — see §4.3.
4. Prices are signed integer ticks ($0.005 each). Order ids fit in 32 bits for
   the standard workload; use 64-bit types anyway.
5. **If you export the optional `engine_prebuild` hook, it may do exactly one
   thing: translate the ABI struct into your engine's native order *value*
   (plus, at most, a one-time capacity `reserve`/`resize`).** Allocating the
   resting node, inserting into the book, running any matching, or populating an
   id→handle map there hoists matcher work off the clock — the harness detects
   it and gates the run INVALID. See §4.6.

**Threads are allowed.** Match and report on whatever thread(s) reflect how the
engine runs in production. The harness records the thread count but never
fails an engine for using threads.

---

## 3.5 Common engine shapes (read before you pick a template)

Most adapter rewrites are forced by the same handful of mismatches between
the engine's API and what the harness expects. Identify which apply to the
user's engine *before* writing the adapter:

- **Fill callback fires twice per fill (once per side).** chronoxor/CppTrader
  does this — first call passes the maker, second passes the taker, with the
  same `(price, qty)`. The adapter must pair consecutive callbacks and emit
  **one** Trade report per fill, not two. See
  `additional_references/cpptrader_adapter/cpptrader_adapter.cpp` for the
  pairing pattern.

- **Engine doesn't expose maker / taker ids in its fill callback.** jxm35 is
  the canonical example — the engine declares `notify_trade(maker_id,
  taker_id, ...)` but `TryMatch` never calls it. Patch the engine source as
  a build step to inject a per-fill hook the adapter can implement (see
  §4.5).

- **Engine has no native IOC support.** Submit the order as a normal limit,
  let it match what it can, then explicitly cancel any leftover — the
  template already shows the residual-cancel + `CancelAck` pattern.

- **Engine matches asynchronously on a background thread.** `engine_flush()`
  must block until the matcher's pipeline is fully drained AND every report
  has been pushed to the transport. The harness's drainer thread reads from
  the transport on its own core, so a transport push from the engine's
  matcher thread is fine — what's *not* fine is returning from
  `engine_flush()` while reports are still in flight inside the engine.

- **Engine returns a fill list rather than calling back.** Skip the
  callback; in `engine_on_new_order`, loop the returned fills, push one
  `ME_TRADE` per fill (same five fields), and decrement `g_taker_left` in
  the loop. See §4.2.

- **Engine prices are doubles or in cents, not int64 ticks.** Convert at the
  ABI boundary. The harness uses `int64_t price_ticks` where one tick is
  $0.005 (the SEC sub-penny). Converters: `int64_t ticks_from_double(double
  p) { return llround(p / 0.005); }`; `int64_t ticks_from_cents(int64_t c)
  { return c * 2; }`. Pick a snap rule (`llround` is the conservative one)
  and apply it consistently on input *and* on the report side (`Trade
  price_ticks`).

- **Engine has its own SPSC report queue and you want it timed.** Export
  `engine_get_transport` instead of pushing into the harness default — see
  §4.6.

When in doubt, find the closest match in `additional_references/` and read
its adapter end-to-end before writing yours; the forty examples cover most
real-world combinations of the points above.

---

## 4. Write the adapter

Create `adapters/<engine>_adapter.cpp` (pick a short lowercase `<engine>`
name). Start from this template and replace every `TODO`:

```cpp
/*
 * <engine>_adapter.cpp — connects <engine> to the matching-engine benchmark.
 */
#include "matching_engine_api.h"   // the harness ABI (compile with -I api)
// TODO: #include the user's engine headers.

#include <cstdint>
#include <unordered_map>

namespace {
// TODO: a single global instance of the user's order book / engine.

const me_transport_t* g_transport = nullptr;   // report transport
void*                 g_sink      = nullptr;   // transport handle

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin: queue full */ }
}

// Shadow of each accepted order. Needed because CancelAck and ModifyAck must
// echo the resting order's `side` and `price_ticks` (the canonical hash
// includes them), and most engines don't surface those fields when you cancel
// by id. We also use it to answer "is this order resting?" — the answer
// distinguishes a CancelAck/ModifyAck from a CancelReject/ModifyReject.
struct OrderState {
    int64_t  price_ticks;
    uint32_t qty;       // current resting quantity
    uint8_t  side;      // 0 = buy, 1 = sell
    bool     alive;     // false once cancelled or fully filled
};
std::unordered_map<uint64_t, OrderState> g_orders;

// Per-call context for the fill callback (taker = current aggressive order):
uint64_t g_seq        = 0;   // aggressor's sequence_number (also goes on Trades)
uint64_t g_taker_id   = 0;   // aggressor's order_id
uint32_t g_taker_left = 0;   // aggressor's UNFILLED quantity (drives IOC residual)

// If the engine reports fills via a callback, the callback emits one Trade
// per fill AND updates both sides' bookkeeping. The taker_left counter is how
// we know whether an IOC order has any residual to cancel after the match.
void on_fill(uint64_t maker_id, uint64_t taker_id,
             int64_t maker_price, uint32_t qty) {
    me_report_t r{};
    r.type            = ME_TRADE;
    r.sequence_number = g_seq;            // the aggressor's seq
    r.price_ticks     = maker_price;      // the MAKER's resting price
    r.quantity        = qty;
    r.maker_order_id  = maker_id;
    r.taker_order_id  = taker_id;
    push_report(r);

    // Track the taker's residual for the IOC drop in engine_on_new_order.
    if (g_taker_left >= qty) g_taker_left -= qty;

    // Decrement the maker's shadow; mark dead once fully consumed.
    auto it = g_orders.find(maker_id);
    if (it != g_orders.end()) {
        if (it->second.qty > qty) it->second.qty -= qty;
        else { it->second.qty = 0; it->second.alive = false; }
    }
}
}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* t, void* sink) {
    g_transport = t;
    g_sink      = sink;
    g_orders.reserve(1u << 21);              // ~2M-message canonical workload
    // TODO: construct the engine / an empty order book for one symbol.
}

void engine_shutdown(void) {
    // TODO: destroy the engine.
    g_orders.clear();
}

void engine_flush(void) {
    // A synchronous engine has nothing pending — leave this empty.
    // An async engine must block here until fully drained (see §4.3).
}

void engine_on_new_order(const new_order_t* o) {
    g_seq        = o->sequence_number;
    g_taker_id   = o->order_id;
    g_taker_left = o->quantity;

    // OrderAck first (the engine has accepted the new order).
    me_report_t ack{};
    ack.type = ME_ORDER_ACK; ack.sequence_number = o->sequence_number;
    ack.order_id = o->order_id; ack.side = o->side;
    ack.price_ticks = o->price_ticks; ack.quantity = o->quantity;
    push_report(ack);

    // TODO: submit the order to the engine; each fill calls on_fill(...).
    //   The match call returns when no more matching is possible on this
    //   message; g_taker_left then holds whatever quantity is unfilled.

    if (o->ioc) {
        // IOC residual: cancel any leftover, emit ONE CancelAck for it.
        if (g_taker_left > 0) {
            // TODO: tell the engine to drop the IOC residual.
            me_report_t r{};
            r.type = ME_CANCEL_ACK; r.sequence_number = o->sequence_number;
            r.order_id = o->order_id; r.side = o->side;
            r.price_ticks = o->price_ticks; r.quantity = g_taker_left;
            push_report(r);
        }
    } else if (g_taker_left > 0) {
        // GTC residual: the leftover rests. Record it so future cancel/modify
        // messages can find its side and price.
        g_orders[o->order_id] = { o->price_ticks, g_taker_left, o->side, true };
    }
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_orders.find(c->order_id);
    me_report_t r{};
    r.sequence_number = c->sequence_number;
    r.order_id        = c->order_id;
    if (it != g_orders.end() && it->second.alive) {
        // TODO: cancel c->order_id in the engine.
        r.type        = ME_CANCEL_ACK;
        r.side        = it->second.side;        // echo the resting order's side
        r.price_ticks = it->second.price_ticks; // echo its price
        it->second.alive = false;
        it->second.qty   = 0;
    } else {
        // Not resting — already filled, already cancelled, or never seen.
        // The canonical workload injects ~2% duplicate cancels that land here.
        r.type = ME_CANCEL_REJECT;
    }
    push_report(r);
}

void engine_on_modify(const modify_t* m) {
    g_seq        = m->sequence_number;
    g_taker_id   = m->order_id;
    g_taker_left = m->new_quantity;
    auto it = g_orders.find(m->order_id);
    if (it != g_orders.end() && it->second.alive) {
        // Modify = cancel + reinsert. The reinsertion at the new price MAY
        // itself cross resting orders on the opposite side and produce Trades
        // (e.g. a buy modify to a higher price that's now marketable). Each
        // crossing fill fires on_fill(...) before we emit the ModifyAck.
        // TODO: cancel m->order_id in the engine, then submit a fresh order
        // at (m->new_price_ticks, m->new_quantity, m->side); on_fill fires for
        // each crossing fill the reinsert produces.
        it->second.alive = false;
        if (g_taker_left > 0) {
            g_orders[m->order_id] = { m->new_price_ticks, g_taker_left, m->side, true };
        }
        me_report_t r{};
        r.type = ME_MODIFY_ACK; r.sequence_number = m->sequence_number;
        r.order_id = m->order_id; r.side = m->side;
        r.price_ticks = m->new_price_ticks; r.quantity = m->new_quantity;
        push_report(r);
    } else {
        // Not resting — already filled, cancelled, or never seen (the canonical
        // workload includes occasional stale modifies).
        me_report_t r{};
        r.type = ME_MODIFY_REJECT; r.sequence_number = m->sequence_number;
        r.order_id = m->order_id;
        push_report(r);
    }
}

int64_t engine_query_best_bid(void) {
    // TODO: highest bid price in ticks, or INT64_MIN if there are no bids.
    // Must reflect the LIVE engine book — the audit catches a stale shadow.
    return INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    // TODO: lowest ask price in ticks, or INT64_MAX if there are no asks.
    return INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    // TODO: total resting quantity at this (price, side); 0 if empty.
    return 0;
}

}  // extern "C"
```

### 4.1 Worked examples to copy from

Pick the closest and read it — they are complete, working adapters:

- `adapters/liquibook_adapter.cpp` — a C++ engine that reports fills through a
  listener **callback** (the pattern above).
- `adapters/quantcup_adapter.cpp` — a C++ engine with a fill callback and a
  small shadow map for the audit queries.
- `adapters/exchange_core_adapter.cpp` + `adapters/HarnessExchangeCore.java` —
  a **JVM** engine reached over JNI (see §4.4).
- `additional_references/` — forty worked adapter examples (twelve C++, ten Rust,
  eight Go, five Java, three Python, one TypeScript, one C) wrapping third-party
  matching engines. Each subdirectory has
  its own README; see `additional_references/README.md` for the index and
  `CORRECTNESS_FINDINGS.md` for the correctness observations the harness
  produced against them.

### 4.2 If the engine returns a fill list instead of using a callback

Skip `on_fill`; in `engine_on_new_order`, loop the returned fills and push one
`ME_TRADE` report for each (same five fields), in match order.

### 4.3 engine_flush and threads

The harness calls `engine_flush()` once, after the last message, **inside the
timed window**. It must not return until every message has been fully matched
and every report pushed. If the engine matches synchronously on the calling
thread, leave `engine_flush()` empty. If it matches or reports on its own
thread(s), block here until those queues are empty. Deferring work past
`engine_flush()` does not make the engine look faster — the flush is timed.

### 4.4 Non-C/C++ engines

The ABI is C, so the engine must be reachable from C++:

- **JVM (Java/Kotlin/Scala):** mirror `adapters/exchange_core_adapter.cpp` +
  `adapters/HarnessExchangeCore.java` — the C++ adapter embeds a JVM via JNI and
  calls a thin Java helper class.
- **Rust / Go / C# / etc.:** export a C ABI from the engine (e.g. Rust
  `#[no_mangle] extern "C"`), then the adapter calls those C functions.

Any engine reached across a runtime boundary that charges a per-call entry cost
(Go/cgo, Java/JNI) should also export **`engine_on_batch`** (§4.6) so the harness
crosses that boundary once per run instead of once per message — otherwise the
fixed per-crossing cost dominates and the throughput you measure is the language
boundary, not the matcher.

### 4.5 Patching upstream source

If the engine is missing something the harness needs — a per-fill hook with
maker / taker ids, a POSIX port — patch the source
as a build step rather than forking it. Convention: `git reset --hard
<pinned_sha>` first (so the patch starts from a known state and re-running
is idempotent), then apply via `sed` or a short Python script. Detect the
already-patched state with a unique post-patch substring so re-runs become
no-ops. Always pin the upstream SHA in `build.sh` so the patch context
doesn't drift. Worked examples:

- `additional_references/jxm35_adapter/build.sh` — Python-patches
  `OrderBook.cpp` to inject `__jxm35_adapter_trade_hook` inside `TryMatch`,
  recovering per-fill maker/taker ids.
- `additional_references/robaho_adapter/build.sh` — `sed`-patches two
  headers for a C++20 conformance fix (`std::vector<const std::string>` →
  `std::vector<std::string>`).
- `additional_references/tzadiko_adapter/build.sh` — `sed`-patches a
  Windows-only `localtime_s` call to its POSIX equivalent, and switches the
  engine's two FillAndKill tail-cancel sites from the locking public
  `CancelOrder` to its own already-locked `CancelOrderInternal` (as shipped,
  the tail self-deadlocks on the mutex `AddOrder` already holds the first
  time an IOC partially fills).

Document each patch in `build.sh`'s comment header and in the adapter's
README so a reader can audit what changed against upstream.

### 4.6 Optional ABI hooks

Three symbols in `api/matching_engine_api.h` are optional — export only if
useful, omit otherwise:

- **`engine_prebuild(uint8_t msg_type, const void* msg)`** — the harness
  calls it once per workload message, in dispatch order, **before** the timed
  window opens. Its **one permitted job is translating the ABI struct into your
  engine's native order *value***: the field marshaling a real gateway does
  before handing the matcher a parsed order — copy/scale the price, set
  side/quantity/flags, pack your id into the native struct. You may also
  pre-size a fixed-capacity table (a one-time `reserve`/`resize` — the
  static-allocation parity a flat-array engine gets at init). The timed
  `engine_on_*` call then takes the pre-built value and does the real work.
  `adapters/quantcup_adapter.cpp` shows the pattern: prebuild fills a `g_pre[]`
  of `t_order` **values**, and the timed `engine_on_new_order` passes the next
  one to `limit()` — which is where the arena slot, the id, and the match all
  happen. The hoist is worth a few percent when the conversion is non-trivial.

  **Nothing else may happen in prebuild, and the harness enforces it.** Prebuild
  must **not** allocate the resting order node, insert into the book, run any
  matching, or populate an id→handle map — each is matcher work that belongs on
  the clock, and hoisting it would make the timed number a lie. Two guards catch
  the variances (full detail in `docs/ANTI_CHEAT.md`):

  1. **Book-empty pre-flight.** Right after the prebuild pass and before the
     clock starts, the harness asserts your book is empty
     (`engine_query_best_bid() == INT64_MIN` and `engine_query_best_ask() ==
     INT64_MAX`). If prebuild rested any order, the book is non-empty here →
     `Anti-cheat: pre-start book not empty by the API sentinels` → **INVALID**, whatever the hash.
  2. **Prebuild-time bound.** The harness times the prebuild pass and compares
     it to the timed run. Honest translation is a small fraction of matching
     (the baselines land at 0.02–0.53×), so a prebuild that *rivals* the run is
     the signature of matcher work hidden there — e.g. a "shadow" pre-matcher
     that matches into a private structure (keeping the queryable book empty to
     slip past guard 1) and replays cached results on the clock. Above 2× the
     harness prints a loud `Anti-cheat: pre-build ran Nx the timed window` flag;
     above 4× — a level no honest translation reaches — it gates the run
     **INVALID**.

  Two practical consequences. **(a)** If your native "order" *is* the
  heap-resident book node — so building it is itself an allocation, as with a
  `new`-per-order engine — construct it in the timed `engine_on_*` call, **not**
  in prebuild; `adapters/liquibook_adapter.cpp` does exactly that (prebuild only
  pre-sizes; the `SimpleOrder` and its id maps are built on the clock). **(b)**
  The one variance neither guard can see is order-*independent* work in your own
  memory — pre-allocating one node per order without resting it leaves the book
  empty *and* costs about as little as translation. Don't: it breaks the same
  contract and the adapters are source-auditable. The contract is the rule;
  the guards are the backstop.
- **`engine_get_transport(void)`** — return the engine's own SPSC transport
  vtable (`create / push / drain / flush / destroy`) instead of accepting
  the harness default (`boost::lockfree::spsc_queue`). Useful when the
  engine already emits into its own lock-free ring and you want the harness's drainer to consume from that
  directly. If the symbol isn't exported, the harness uses its default and
  hands the vtable + handle to `engine_init`.
- **`engine_on_batch(const me_msg_t* msgs, uint32_t n)`** — accept the workload
  as runs of `n` tagged messages instead of one `engine_on_*` call per message,
  so the harness crosses the ABI boundary once per run rather than once per
  message. Each message is still processed exactly as if delivered alone, in
  array order, with **no cross-message lookahead** (the contract — and how the
  hash + state audit enforce it — is in `docs/METHODOLOGY.md` "Batch delivery"
  and the CONTRACT comment in `api/matching_engine_api.h`). The win is for
  engines behind a language runtime (§4.4), where the per-crossing cost is real;
  a native C/C++ engine gains little. If the symbol isn't exported, the harness
  drives the engine one message at a time as before.

If you don't export any of these, the harness uses defaults; you lose nothing
correctness-wise, only the small perf hoist that prebuild provides.

---

## 5. Build the adapter

Compile the adapter **and** the engine's sources into one `.so`. Name the
output `<engine>_adapter.so` and place it at the repository root.

```sh
g++ -std=c++20 -O3 -march=native -fPIC -shared -I api \
    adapters/<engine>_adapter.cpp \
    path/to/engine/file1.cpp path/to/engine/file2.cpp \
    -I path/to/engine/include \
    -o <engine>_adapter.so
```

Notes:
- `-I api` is required (it finds `matching_engine_api.h`).
- `-march=native` is the project-wide default (matches `scripts/build_baselines.sh`
  and `docs/INTEGRATION.md`); it lets the compiler use the host's instruction
  set. For a non-C++ engine use the equivalent (`RUSTFLAGS="-C target-cpu=native"`
  for Rust cdylibs; Go's `c-shared` mode has no equivalent flag).
- Match the C++ standard the engine itself needs.
- If the engine spawns threads or uses `std::thread`, add `-lpthread`.

---

## 6. Run the harness

```sh
./harness --engine ./<engine>_adapter.so --scenario normal --mode perf
./harness --engine ./<engine>_adapter.so --scenario normal --mode audit
```

`reference/correctness_hash.txt` ships a published hash for every scenario at
the canonical seed 23; `normal` is the canonical byte-identical consensus, so validate
against `normal` first. A `perf` run measures throughput; an `audit` run runs
the anti-cheat state audit. `scripts/run_challenge.py` drives the full
protocol — 10 perf runs + 1 audit run per scenario — and prints the per-scenario
median throughput, the overall verdict, and (over all five scenarios, the default)
the worst-case throughput as the definitional result.

---

## 7. Read the result

Look at the final lines. **Success is `Verdict: VALID`** with `Status: PASS`
(and, on an audit run, `State audit: PASS`).

If it is not VALID, diagnose with this table:

| Symptom | Cause | Fix |
|---|---|---|
| `missing required symbol` / `undefined symbol` | Not all functions exported, or not `extern "C"` | Wrap every function in `extern "C" { ... }`; export all of them, including `engine_flush`. |
| `dlopen(...): cannot open shared object` | Wrong `.so` path | Check the path passed to `--engine`. |
| `Status: FAIL` | Engine emits a wrong report stream | The Trade `price_ticks` must be the **maker's** resting price; `sequence_number` must be the **aggressor's**; IOC residuals must be **dropped**, not rested; check `maker_order_id` / `taker_order_id` and that fills are pushed in match order. |
| `State audit: FAIL` | `engine_query_*` do not reflect the real book | Implement best-bid / best-ask / depth against the live book; return `INT64_MIN` / `INT64_MAX` when a side is empty. |
| `engine crashed (fatal signal)` | Adapter bug (segfault) | Check the engine handle is initialised and the transport pointers are saved in `engine_init`. |
| `Affinity: matcher=FAILED` or `drainer=FAILED` (verdict INVALID despite `Status: PASS`) | The harness couldn't pin the matcher / drainer thread to the requested core | Pass `--matcher-core <N>` and `--drainer-core <M>` with cores that are online on your box. The default 2 / 3 may not exist (small machines) or may not be reachable from the harness's CPU set; pick any two adjacent cores >= 0. |
| `Status: NO REFERENCE` | The (scenario, seed) pair has no published hash in `reference/correctness_hash.txt` (e.g. a custom seed or scenario) | The five standard scenarios at the canonical seed 23 all have published hashes — if you're seeing NO REFERENCE on `normal` at seed 23, the reference file is missing or corrupted. On a custom (scenario, seed) the harness has nothing to compare against and the run is reported INVALID; either pick a published (scenario, seed) or add an entry to `reference/correctness_hash.txt` via `./harness --baseline liquibook --scenario <name> --seed <n> --write-reference`. |

On a `Status: FAIL`, the engine's report stream differs from
`reference/canonical_output.txt.gz` — the canonical text that hashes to the
reference, one line per report (the per-type line format is in
`docs/METHODOLOGY.md`). The shipped `.gz` covers `normal` only; for the four
other scenarios regenerate locally first:

```sh
./harness --baseline liquibook --scenario <scn> --seed 23 --write-reference
# writes reference/canonical_output_<scn>.txt
```

Decompress `normal` with `gunzip -k reference/canonical_output.txt.gz`, then
temporarily log each report your adapter emits and diff against the
canonical file to find the first divergence. (Do **not** regenerate the
canonical with `--write-reference` from your engine under test — that would
overwrite the reference with your engine's potentially-wrong output.
`--write-reference` only accepts `--baseline`.)

---

## 8. Success checklist

The connection is built and running correctly when **all** of these hold:

- [ ] `--mode perf` on `normal`: `Status: PASS`, `Verdict: VALID`.
- [ ] `--mode audit` on `normal`: `State audit: PASS`, `Verdict: VALID`.
- [ ] The process exit code is `0`.

Then exercise every scenario and (optionally) compare against the baselines:

```sh
scripts/run_challenge.py --engine ./<engine>_adapter.so --all-scenarios
scripts/build_baselines.sh all      # builds liquibook, quantcup, exchange_core
scripts/run_challenge.py --compare <engine> liquibook quantcup exchange_core
```

Report the median throughput (M msgs/s) and the verdict for each scenario back
to the user. A result is only meaningful if its verdict is `VALID`.
