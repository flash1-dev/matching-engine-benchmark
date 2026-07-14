# pyme_adapter — integration example

Wraps [Surbeivol/PythonMatchingEngine](https://github.com/Surbeivol/PythonMatchingEngine)
("pyme") behind `api/matching_engine_api.h`. pyme is a pure-Python matching
engine (price-time priority, FIFO doubly-linked lists per price level), so there
is no native engine library to link: the adapter embeds CPython and drives the
engine's `Orderbook` class through the C-API.

Pinned commit:
- `Surbeivol/PythonMatchingEngine` — `f94150294a85d7b415ca4518590b5a661d6f9958`

This adapter is one of the worked examples in `additional_references/` — none
are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this snapshot
(pyme: no fix required).

## Engine shape

`Orderbook` facade, single-threaded synchronous matcher. Native APIs the adapter
uses:

- `Orderbook.send(is_buy, qty, price, uid, is_mine=True)` — submits a limit
  order; matches it against the opposite side and appends each fill to
  `ob.trades` (`price` = MAKER resting price, `agg_ord` = taker uid,
  `pas_ord` = maker uid), in match order; any residual rests.
- `Orderbook.cancel(uid)` — native cancel-by-id of a resting order.
- `Orderbook._orders[uid]` — the engine's own `uid -> Order` table, used as the
  authoritative "is this order resting?" oracle (`Order.active`) and to echo a
  resting order's side/price on a CancelAck.
- `Orderbook.best_bid` / `best_ask` / `_bids.book` / `_asks.book` — book
  snapshots for the harness state-audit queries.

Not provided natively: no IOC / FOK / POST-ONLY flag; no price-changing modify
(`Orderbook.modif` only downsizes quantity in place); the engine emits no
explicit Ack / Reject events (the adapter synthesises those from native state).

## Adapter strategy

The C++ side (`pyme_adapter.cpp`) embeds CPython once in `engine_init`, imports a
thin orchestration module (`pyme_driver.py`), and bridges each `engine_*` ABI
call to a driver function. The driver calls the engine's native `Orderbook` API
and **never reimplements matching**; it returns, per message, a list of report
tuples `(rtype, side, seq, order_id, price_ticks, quantity, maker_id, taker_id)`
which the C++ side unpacks and pushes into the harness report transport.

- **Reports come from the engine.** A Trade is emitted for each row pyme appended
  to `ob.trades`; `Trade.price_ticks` is the maker's resting price (`ob.trades`
  already stores the maker price) and `Trade.sequence_number` is the aggressor's.
- **Prices**: the harness uses signed integer ticks; pyme keys its book by float
  price. The driver passes `float(price_ticks)` (integer-valued, exact in IEEE
  double over the workload range) and converts the maker price back with
  `int(round(px))`. `is_mine=True` on every `send` bypasses the engine's
  historical-order market-impact path (it only rewrites prices when
  `is_mine=False`), so prices stay pristine.
- **Order-id mapping**: none needed — pyme names orders with the harness's own
  `uid`, so the harness id is passed straight through as `Order.uid`. No
  adapter-side id→handle map is built.
- **IOC**: submit as a normal limit, let it match, then `Orderbook.cancel` the
  rested residual and emit one CancelAck (the engine has no IOC flag).
- **Modify**: native cancel + reinsert at the new price/qty (loses time
  priority; the reinsert may itself cross and print Trades), then one ModifyAck;
  ModifyReject if the order is not resting. pyme's in-place `modif` is
  quantity-down-only, so it cannot express a price change.
- **Cancel / modify of a non-resting order** (already filled, already cancelled,
  or never seen) → CancelReject / ModifyReject, adjudicated via
  `_orders[uid].active`, i.e. the engine's own table — no adapter shadow book.

`engine_flush` is a no-op: matching and report emission complete synchronously
inside each `engine_on_*` call, so the pipeline is already drained. The adapter
exports no `engine_prebuild`, so the harness runs no prebuild pass (nothing is
inserted into the book ahead of the timed run). The harness reports two engine
threads "after_init"; these are the embedded interpreter's own runtime threads,
not matcher workers — all matching runs on the harness's matcher thread under
the GIL (acquired once at init and held for the whole run — there is no per-call
`PyGILState_Ensure` on the hot path; it is released at `engine_shutdown`).

## Source patch

No source patch. pyme already exposes per-order ids, native cancel-by-id,
crossing at the maker (resting) price with maker/taker ids recorded per fill, and
an authoritative `Order.active` flag, so the engine is built and run unmodified.
`CORRECTNESS_FINDINGS.md` records it as conforming ("No fix required").

## Build / run

```bash
bash additional_references/pyme_adapter/build.sh
./harness --engine pyme_adapter.so --scenario normal --mode audit \
          --matcher-core 60 --drainer-core 61
```

`build.sh` clones the engine into `third_party/pyme_python_matching_engine/` at
the pinned commit (no patch, just `git reset --hard`), then compiles
`pyme_adapter.cpp` against the embeddable system CPython
(`python3-config --embed`) into `pyme_adapter.so` at the repo root. The engine
clone dir and this adapter dir are baked into the `.so`'s `sys.path`
(`-DPYME_REPO_DIR` / `-DPYME_ADAPTER_DIR`) so it loads regardless of the
harness's working directory.

Requirements: an embeddable system `python3` (the `python3-dev` headers, for
`python3-config --embed`) plus the engine's runtime deps `numpy`, `pandas`,
`pyyaml` importable by that interpreter. `build.sh` `pip install --user`s any of
the three that are missing; the embed headers cannot be auto-installed without
root and must already be present. Override the upstream checkout with
`ME_PYME_SRC=/path/to/existing/clone` (the dir must contain `marketsimulator/`).
