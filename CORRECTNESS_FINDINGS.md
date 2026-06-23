# Correctness findings

This document is the harness's **correctness record**: whether each engine's
output conforms to price-time priority, the one-line mechanism of any divergence,
and the upstream issue filed for it. It spans the **whole audited set of 63
distinct engines** driven through the harness — fifty-five consensus-conforming
(every one reproduces the byte-identical consensus: FlashOne the publisher's
target, the three engines that first established the published reference hash —
Liquibook, QuantCup, Exchange-core — and the rest VALID as shipped or after a
documented engine fix) and eight
non-conforming. "Conforming" means an engine reproduces the consensus on the
canonical workload, passes the pre-run conformance gate
([`docs/CONFORMANCE.md`](docs/CONFORMANCE.md)), and carries no known latent defect.

The harness surfaced correctness reports covering **60+ distinct defects** across
that set. **42 are now filed upstream** (several already fixed by their maintainers
— geseq and CppTrader are documented in [`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md),
and matchingo and cheetah were fixed upstream after we filed); two more are prepared but could not be filed (the repo is
archived or has issues disabled); two were already reported by others; the
remainder are *latent* (conform on the canonical workload, carry a bug off it),
*representational-limit*, or *deferred* findings. The roster below carries the
filed-issue link and a one-line finding per engine — each issue holds the full
mechanism, reproduction, and suggested fix that this document previously spelled
out inline.

This document is a snapshot, not a judgment. Each observation describes the
upstream commit listed in [`SNAPSHOTS.md`](SNAPSHOTS.md) (or, for the wider-audit
engines, the commit named in that finding); a project's current `main` may already
differ. **We draw no conclusion about engineering quality or fitness for any
specific use case; the projects' designs reflect their authors' goals, which may
differ from ours.** Every finding here is offered back, not aimed at anyone — a
reproducible, time-stamped snapshot of a specific commit.

## How the harness probes correctness

Two correctness signals per engine per scenario:

1. **Report-stream hash** — SHA-256 over the engine's full output stream
   (OrderAck, Trade, CancelAck, ModifyAck, CancelReject, ModifyReject)
   stable-sorted by `(sequence_number, type)`, compared against the byte-identical
   consensus the conforming field reproduces (first established from three
   independent engines: Liquibook, QuantCup, Exchange-core).
2. **State audit** — 192 random-point `engine_query_*` checks (64 indices ×
   `best_bid`/`best_ask`/`depth_at`) against a baseline replay.

A run is **VALID** only when the report hash *and* all 192 state probes match; any
byte difference — a different trade price, a different maker id, an extra or a
missing fill — makes it **INVALID** (see `docs/ANTI_CHEAT.md`). Each finding is
tagged by kind: a **hard-invariant violation** (quantity not conserved, a fill
past resting size, a trade through the book), a **price-time-priority violation**
(quantity-conserving, but a wrong execution price or counterparty), or
**engine-state corruption**. INVALID means only "this engine's output diverges
from the byte-identical consensus," not a judgment of engineering quality.

## Per-engine roster

The verdict for every engine driven through the harness — the **full roster of
63**: fifty-five consensus-conforming — every one reproduces the byte-identical
consensus (FlashOne the target, the three engines that first established the
published reference hash, and the rest VALID as shipped or after a documented
fix) — and eight non-conforming. In
the conforming table the Verdict reads **`No fix required`** when the engine
reproduces the consensus byte-for-byte and passes the state audit on all five
scenarios as shipped (and, for the audited set, across 100 further random seeds),
and **`Fix submitted upstream`** / **`Fix prepared`** when it does so only after
the minimal documented patch named (the upstream issue filed, or prepared but
unfileable); `latent` is VALID on the canonical run with a real bug off it;
`INVALID` diverges on the canonical workload, `INVALID (state
audit)` passes the report hash but fails the 192-point probes. The **`Issue`
column links the filed upstream report** — where the full mechanism, repro, and
suggested fix now live; `resolved` → [`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md),
`—` is a clean engine. Two findings are prepared but unfileable (repo archived /
issues disabled) and two were already reported by others, as the `Issue` cell notes.

### Consensus-conforming engines

| Engine        | Language   | Verdict        | Divergence / finding (one line) | Issue |
|:--------------|:-----------|:---------------|:--------------------------------|:------|
| FlashOne      | C++        | No fix required       | reference target — the harness publisher's production engine | — |
| cpp-orderbook | C++        | No fix required    | — (pinned commit already carries the price-cross fix) | resolved |
| CppTrader     | C++        | No fix required       | — on canonical; a `ModifyOrder` crash off the canonical path is fixed upstream | resolved ([#42](https://github.com/chronoxor/CppTrader/issues/42)) |
| llc993        | Rust       | No fix required   | — (BTreeMap + slab pool + intrusive time-queue) | — |
| hroptatyr/clob | C         | No fix required       | — (b+tree CLOB, `_Decimal64`, no patch) | — |
| mercury       | C++        | No fix required       | — (abseil b-tree) | — |
| microexchange | C++        | No fix required      | — (array + bitmap) | — |
| Tzadiko       | C++        | Fix submitted upstream | self-deadlocks on a partially-filled IOC; two-site `CancelOrderInternal` patch → VALID ×5 (+ teardown lost-wakeup, filed separately) | [#11](https://github.com/Tzadiko/Orderbook/issues/11) + [#12](https://github.com/Tzadiko/Orderbook/issues/12) |
| coralme       | Java       | No fix required       | — | — |
| gocronx       | Rust       | No fix required       | — | — |
| geseq         | Go         | No fix required    | — (a multi-level cross-through is fixed upstream) | resolved ([#25](https://github.com/geseq/orderbook/issues/25)) |
| Exchange-core | Java/JVM   | No fix required      | consensus anchor (direct-access book, JNI per message) | — |
| jiang         | Java       | Fix submitted upstream | `cancelOrder` never prunes the id index → modify drops orders then crashes; 1-line `idMaps.remove(id)` → VALID ×5 | [#3](https://github.com/JiangYongKang/FastMatchingEngine/issues/3) |
| m5487         | Go         | No fix required       | — (skiplist + disruptor) | — |
| jlob          | Java/JNI   | No fix required      | — (L3 RB-tree, working JNI adapter) | — |
| mh2rashi      | C++        | Fix submitted upstream | `deleteOrder` guard nulls a 2-order level's survivor → list corruption/crash on all five; 1-line fix → VALID ×5 | [#4](https://github.com/mh2rashi/Trading-Engine/issues/4) |
| QuantCup      | C++        | No fix required       | consensus anchor; flat price-indexed array, domain widened to 32-bit (see note below) | — |
| pyme          | Python     | No fix required      | — (doubly-linked price levels, CPython embed) | — |
| brprojects    | C++        | No fix required       | — (uncached-height AVL) | — |
| dyn4mik3      | Python     | Fix submitted upstream | `get_volume_at_price` calls a non-existent `get_price` → crash on depth query; 1-line fix → VALID ×5 | [#22](https://github.com/dyn4mik3/OrderBook/issues/22) |
| trademacher   | Java/JNI   | No fix required       | — | — |
| jeog          | C++        | No fix required    | — (flat directly-indexed price vector) | — |
| Liquibook     | C++        | No fix required       | consensus anchor; native IOC residual rests instead of cancelling | [#43](https://github.com/enewhuis/liquibook/issues/43) |
| philipgreat   | Rust       | Fix submitted upstream | three cancel/modify-path issues (zero-qty phantoms, tombstone cancel, id-index disown); three patches → VALID ×5 | [#1](https://github.com/philipgreat/lighting-match-engine-core/issues/1), [#2](https://github.com/philipgreat/lighting-match-engine-core/issues/2), [#3](https://github.com/philipgreat/lighting-match-engine-core/issues/3) |
| cointossx     | Java/JNI   | Fix submitted upstream | B+Tree destructive `firstKey` (best collapses to 0 past 100 levels) + `AddOrderPreProcessor` wrong-side compare; 2 fixes → VALID ×5 | [#10](https://github.com/dharmeshsing/CoinTossX/issues/10) |
| auralshin     | Rust       | No fix required       | — | — |
| fasenderos    | TypeScript | No fix required       | — (libnode embed; very slow in its weakest scenario) | — |
| lobster       | Rust       | No fix required       | — (BTreeMap + arena + id-index; very slow in its weakest scenario) | — |
| techieboy     | Rust       | Fix submitted upstream | spurious zero-qty fills + stale best-bid/ask; 2 fixes → VALID ×5 | [#1](https://github.com/TechieBoy/rust-orderbook/issues/1) |
| piyush       | C++      | Fix submitted upstream | report stream byte-identical on all five; asymmetric cached best-ask staleness fails the state audit on the moving scenarios — fix verified, VALID ×5 across 100 seeds | [#9](https://github.com/PIYUSH-KUMAR1809/order-matching-engine/issues/9) |
| limitbook    | Rust     | Fix submitted upstream | partial fill never decrements the resting maker → ~4.3× over-match (quantity not conserved) — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/solarpx/limitbook/issues/1) |
| robaho       | C++      | Fix submitted upstream | trade priced at the aggressor's limit, not the maker's (price-priority); `static` passes — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/robaho/cpp_orderbook/issues/2) |
| jxm35        | C++      | Fix submitted upstream | cancel-path double-unlink corrupts the level → missed crossings + bad state; trade hook never invoked — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/1), [#2](https://github.com/jxm35/LimitOrderBook-MatchingEngine/issues/2) |
| OrderBook-rs | Rust     | Fix submitted upstream | partial fill demotes the maker to the FIFO tail → wrong counterparty; quantities correct — fix verified, VALID ×5 across 100 seeds | [#88](https://github.com/joaquinbejar/OrderBook-rs/issues/88) |
| mansoor      | C++      | Fix submitted upstream | OOB on a wide swing — unchecked bounded price array crashes on `flash-crash` (clean in-band; VALID on the two scenarios it can finish within budget) — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/mansoor-mamnoon/limit-order-book/issues/3) |
| danielgatis  | Go       | Fix submitted upstream | `decimal.Decimal` Go map key — equal prices become distinct keys, orphaning same-price orders (~61% under-match) — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/danielgatis/go-orderbook/issues/2) |
| lightning    | Go       | Fix submitted upstream | skiplist `Delete` predecessor corruption loses resting orders → cancels/modifies wrongly rejected (nondeterministic) — fix verified, VALID ×5 across 100 seeds | reported upstream (duplicate) |
| apex         | Rust     | Fix submitted upstream | crossing fills priced at the aggressor's limit, not the maker's; `static` passes — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/crypto-zero/apex-engine/issues/3) |
| darkpool     | C++      | Fix submitted upstream | aggressor pricing in the execution-price report — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/dendisuhubdy/dark_pool/issues/1) |
| matchina     | Rust     | Fix submitted upstream | phantom zero-quantity trades (no taker-exhaustion guard in the level loop) — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/fran0x/matchina/issues/3) |
| luo4neck     | C++      | Fix submitted upstream | fills the first time-ordered crossing, not the best price (price-time violation); `static` passes — fix verified, VALID ×5 across 100 seeds | [#3](https://github.com/luo4neck/MatchingEngine/issues/3) |
| cspooner     | Rust     | Fix submitted upstream | partial fill deletes both orders wholesale (quantity not conserved); + wrong-side ask cancel, ask pricing, no time priority, 2dp rounding — fix verified, VALID ×5 across 100 seeds | [#14](https://github.com/christian-spooner/trading-server/issues/14) |
| asthamishra  | Rust     | Fix submitted upstream | direct-indexed array, bounded 100k-tick domain — drops orders above the ceiling on wide swings — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/AsthaMishra/matching-engine/issues/1) |
| pgellert     | Rust     | Fix submitted upstream | `check_for_trades` drops a popped order; stale price-bounds hide marketable orders → ~70% under-match — fix verified, VALID ×5 across 100 seeds | [#2](https://github.com/pgellert/matching-engine/issues/2) |
| matchingo    | Go       | Fix submitted upstream | report stream correct; `UpdateVolume` subtracts the remainder not the consumed qty → depth audit fails — fix verified, VALID ×5 across 100 seeds | [#1](https://github.com/GOnevo/matchingo/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| Kautenja      | C++        | Fix submitted upstream | duplicate live order-id → unchecked `emplace` re-inserts the first order (self-linked FIFO, double-counted volume, UAF on cancel) — fix verified, VALID ×5 across 100 seeds | [#4](https://github.com/Kautenja/limit-order-book/issues/4) |
| fmstephe      | Go         | Fix submitted upstream | crossing trades print at the bid-ask midpoint, not the maker; adapter applies the maker-price correction — fix verified, VALID ×5 across 100 seeds | [#11](https://github.com/fmstephe/matching_engine/issues/11) |
| i25959341     | Go         | Fix submitted upstream | `OrderSide.Volume()` over-reports after a partial fill (per-side aggregate, never read by the harness) — fix verified, VALID ×5 across 100 seeds | reported upstream (duplicate) |
| gotrader     | Go         | Fix submitted upstream | a modify of a fully-filled order is swallowed (acked), not rejected — `ModifyOrder` swallow (conformance gate) — fix verified, VALID ×5 across 100 seeds | [#23](https://github.com/robaho/go-trader/issues/23) |
| oceanbook     | Go         | Fix submitted upstream | `Depth` writes quantity into the price field; off the match path (matching byte-identical) — fix verified, VALID ×5 across 100 seeds | [#44](https://github.com/draveness/oceanbook/issues/44) |
| dsirotkin     | C++        | Fix prepared | cancel range-erases the rest of the price level; only fires off the canonical path — fix verified, VALID ×5 across 100 seeds | drafted — repo archived, unfiled |
| dgtony        | Rust       | Fix submitted upstream | id-gen wraps [1,1000] (1001st order dropped); amend leaves the book crossed — fix verified, VALID ×5 across 100 seeds | [#9](https://github.com/dgtony/orderbook-rs/issues/9) |
| pantelwar     | Go         | Fix submitted upstream | off-path hot-path debug logging + a `MarshalJSON` sell-side bug — fix verified, VALID ×5 across 100 seeds | [#26](https://github.com/Pantelwar/matching-engine/issues/26) |
| pyxchange     | C++        | Fix prepared | wall-clock `(price,time)` key drops same-tick same-price orders — fix verified, VALID ×5 across 100 seeds | drafted — issues disabled, unfiled |
| ridulfo       | Python     | Fix submitted upstream | `LimitOrder.__lt__` is not a consistent total order → priority inversion + lost cancels on time ties — fix verified, VALID ×5 across 100 seeds | [#10](https://github.com/ridulfo/order-matching-engine/issues/10) |

### Non-conforming engines

| Engine       | Language | Verdict             | Divergence / finding (one line) | Issue |
|:-------------|:---------|:--------------------|:--------------------------------|:------|
| femto_go     | Go       | INVALID (moving)    | VALID on `static`/`normal`; diverges deterministically on the three wide-swing scenarios (price-window limit; same hash on re-run, batched == one-at-a-time, so it is the engine's) | — (limitation, not filed) |
| cheetah      | Rust     | INVALID             | cancel searches the opposite book (every cancel fails + leaks); a 10k-id de-dup window drops ~97% of non-monotonic ids | [#1](https://github.com/CheetahExchange/orderbook-rs/issues/1) — resolved upstream ([`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)) |
| lanpishu     | C++      | INVALID             | broken RB-tree delete-fixup → wrong best price/counterparty + under-match (can hang); depth double-decrement → phantom levels | [#2](https://github.com/lanpishu6300/crypto-exchange/issues/2) |
| laffini      | Java     | INVALID (crash)     | crashes all five — `\|\|` should be `&&`, plus missing price-cross / empty-list guards | [#27](https://github.com/Laffini/Java-Matching-Engine-Core/issues/27) |
| piquette     | Go       | INVALID             | bid/ask maps never populated → every cancel rejected; duplicate-price stranding; best-ask uses `>` not `<` | [#2](https://github.com/piquette/orderbook/issues/2) |
| zackienzle   | C++      | INVALID (wide swing) | hierarchical 4-tier bitmap, bounded price domain — crashes (OOB) on `swing-40` / `flash-crash` | — (no draft) |
| mtengine     | Rust     | INVALID (wide swing) | flat array + bitmap, bounded price domain — diverges on `flash-crash` / the widest swings | — (no draft) |
| gavincyi     | Python     | latent              | crashes on a cancel/modify of a fully-filled order — stale `order_id_map` entry → AssertionError (conformance gate) | [#18](https://github.com/gavincyi/LightMatchingEngine/issues/18) |

omerhalid (partial-fill depth over-count) is filed ([#3](https://github.com/omerhalid/Real-Time-Market-Data-Feed-Handler-and-Order-Matching-Engine/issues/3))
but does not appear as a distinct roster row — it surfaced during the audit. Of the
original twelve, CppTrader, geseq, and the author-contributed cpp-orderbook are
patch-free and VALID ×5 but each surfaced a now-resolved defect (see
[`RESOLVED_FINDINGS.md`](RESOLVED_FINDINGS.md)); every other shipped adapter carries
a documented finding.

### A note on a reference engine's representational limit

The published reference hash was first established from three independent engines
(Liquibook, QuantCup, Exchange-core). One carries a correctness-relevant
robustness patch worth recording: QuantCup's flat price-indexed array uses a
fixed-width price domain, and the upstream 16-bit form (a 65,535-slot array)
**aborts at the engine's representational ceiling** when a wide-swing workload
visits a price beyond it. The baseline carries a patch widening the price word to 32 bits and
the flat array to 262,144 slots (2^18) so a wide-swing seed indexes in range instead of
aborting; it is byte-identical on the canonical seed — see `docs/PATCHES.md`.

## Reproducing

Each observation in this document is reproducible from running code in this
repository:

```bash
bash additional_references/<name>_adapter/build.sh   # clones + builds
./harness --engine <name>_adapter.so --scenario normal --mode audit \
          --matcher-core 82 --drainer-core 83
```

Where `<name>` is any of the forty adapters in [`additional_references/`](additional_references/).
The build scripts pin each upstream to
the commits listed in [`SNAPSHOTS.md`](SNAPSHOTS.md). The adapters themselves are not maintained
— if any upstream advances past the pinned commit the source-level
observations here may no longer apply; treat this document as a record of
one point in time.
