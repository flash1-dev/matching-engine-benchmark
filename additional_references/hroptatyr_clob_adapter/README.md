# hroptatyr_clob_adapter — integration example

Wraps [hroptatyr/clob](https://github.com/hroptatyr/clob) (Sebastian Freundt's
"clob": a b+tree-based central limit order book in C with pluggable uncrossing
schemes) behind `api/matching_engine_api.h`.

Pinned commit:
- `hroptatyr/clob` — `812137a3edca4e00f05ac8b3ff2212c5deb545a5` (tag v0.1.0)

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot.

## Engine shape

Plain C API over a `clob_t` (two b+trees, one per side; each price level is a
`plqu` FIFO of orders). Single-threaded, fully synchronous matcher. Native APIs
visible from the adapter:

- `unxs_order(clob_t, clob_ord_t, ref)` — the continuous-trading entry point:
  crosses the incoming order against the contra book in price-time priority,
  records every fill into the book's attached execution stream (`book.exe`, a
  `MODE_BI` `unxs_t` holding `{price, qty}` + maker/taker oids + maker side per
  fill), and returns the residual order.
- `clob_add(clob_t, clob_ord_t)` — rests an order; returns a `clob_oid_t`
  (type, side, price, queue-id, usr) that fully addresses it.
- `clob_del(clob_t, clob_oid_t)` — cancels by that oid; returns `< 0` if absent.
- `clob_aggiter` / `clob_aggiter_next` — aggregated level iterator per side,
  used to answer the best-bid / best-ask / depth queries.

Numeric type: built in the engine's DEFAULT `_Decimal64` (IEEE-754 DFP64)
configuration — the mode `./configure` selects when the compiler has DFP support
(gcc-14/aarch64 does). The workload's integer ticks and quantities are small and
convert exactly to and from `_Decimal64`.

Not provided natively: no id→order index (an order is addressed only by its full
`clob_oid_t`); no IOC / FOK flag on the continuous path; no native modify; no
reject events.

## Adapter strategy

- **New order**: emit `OrderAck`, `unxs_order()` to cross, drain `book.exe` into
  one `Trade` per fill (maker price = the recorded execution price; maker/taker
  ids = the `usr` field stamped on every order), then `clob_add()` the residual
  and remember its oid. The fill stream is read through a struct that mirrors the
  engine's private `_unxs_s` layout — the same fields `cloe.c` iterates — and is
  cleared with `unxs_clr` after each cross.
- **Cancel**: `clob_del()` the remembered oid; `CancelAck` on success,
  `CancelReject` if the order is not resting.
- **Modify**: cancel + reinsert at the new price/qty (losing time priority — the
  harness modify model): `clob_del()` the old oid, then run the new-order
  crossing + rest path; `ModifyAck`, or `ModifyReject` if not resting.
- **IOC**: the residual after crossing is reported as a `CancelAck` and never
  rested (the engine has no IOC flag on the continuous path).
- **Prices**: workload `int64_t` ticks cast directly to/from `_Decimal64`
  (exact for the small integers the workload uses); harness `side` 1 = sell maps
  to `CLOB_SIDE_ASK`.
- **Liveness shadow** (a flat `vector`-style array indexed by the dense harness
  order id): clob offers no id→order index, and its `plqu` queue-ids are
  per-level and **recycled** when a level empties and is freed, so a stale oid
  could spuriously match a different live order at a recycled queue. The adapter
  therefore keeps, per harness order id, the live `clob_oid_t` plus a one-bit
  "resting" flag and adjudicates cancel/modify against that flag — the pattern
  the mansoor / liquibook reference adapters use. The flag is cleared when the
  order is fully filled (the maker remainder the engine reports for a fill is
  zero), cancelled, or modified. The array is sized untimed in `engine_init` /
  `engine_prebuild`, so the hot path only does a bounds-checked load — no
  allocation, no lock; `Trade`/ack reports are the engine's fills pushed straight
  through the transport.

## Source patch

**No source patch.** The engine is compiled byte-for-byte unmodified from its
pinned HEAD in its default `_Decimal64` configuration; this adapter is the only
glue. (`CORRECTNESS_FINDINGS.md`: "No fix required".) `build.sh` runs the
engine's own `autoreconf` + `./configure` only to generate the two headers that
are not committed upstream — `src/config.h` and `src/clob_type.h` (the latter
from the committed `src/clob_type.h.in`) — which pick the DFP encoding and the
price/quantity type. It then compiles the matcher translation units
(`btree.c plqu.c clob.c unxs.c quos.c dfp754_d64.c`) directly into the `.so`
rather than invoking the engine Makefile, which additionally builds the `cloe`
CLI (needs the `yuck` option-parser generator — not installed, not needed) and
the unused 32-bit-decimal TU `dfp754_d32.c`.

The link passes `-Wl,--allow-multiple-definition`: `dfp754_d64.h` declares a few
plain `inline __attribute__((pure, const))` math helpers (e.g. `nand64`) that
GCC-14 emits as an out-of-line external symbol in every TU that includes the
header, so a multi-TU link otherwise reports "multiple definition". Every copy
is byte-identical (same header, same flags, a pure/const function), so taking
the first is exact. This is a linker flag only — the engine source stays
unmodified (the upstream autotools build sidesteps the same collision by linking
the engine as a static archive with selective member extraction).

## Build / run

```bash
bash additional_references/hroptatyr_clob_adapter/build.sh
./harness --engine hroptatyr_clob_adapter.so --scenario normal --mode audit \
          --matcher-core 52 --drainer-core 53
```

`build.sh` clones the engine into `third_party/hroptatyr_clob/` at the pinned
commit (`git reset --hard` to the pin, then the engine's `autoreconf` +
`./configure`), and writes `hroptatyr_clob_adapter.so` to the repository root.
Override: `ME_ENG_SRC=/path/to/existing/clone` uses an existing checkout in
place of cloning. The build needs `autoreconf` (autoconf/automake) and a
DFP-capable C compiler (gcc-14 `_Decimal64`); both are checked with a loud fail.
