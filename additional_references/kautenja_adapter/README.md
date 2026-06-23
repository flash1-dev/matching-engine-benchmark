# kautenja_adapter — integration example

Wraps [Kautenja/limit-order-book](https://github.com/Kautenja/limit-order-book)
behind `api/matching_engine_api.h`. The engine is a header-only,
price-time-priority limit-order book; it pulls three header-only helpers
(`binary-search-tree`, `doubly-linked-list`, `robin-map`) in as git submodules.

Pinned commit:
- `Kautenja/limit-order-book` — `88416a12a0b34b026cbf1d598823fd315a1f2dbf`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

`LOB::LimitOrderBook` (`include/limit_order_book.hpp`), single-threaded matcher,
matches synchronously on the calling thread. Per side: a BST of price `Limit`
nodes (`vendor/binary-search-tree`), each holding a FIFO doubly-linked list of
`Order` nodes (`vendor/doubly-linked-list`), plus a flat `tsl::robin_map` of
price → `Limit*` and a `std::unordered_map<UID,Order>` for cancel-by-id.
Native APIs visible from the adapter:

- `limit(side, uid, qty, price)` — add a limit order; crosses what it can
  against the opposite side, then rests the residual. Matching is triggered
  inside `limit_buy`/`limit_sell`.
- `cancel(uid)` — remove a resting order by id.
- `has(uid)` / `get(uid)` — existence / lookup by id.
- `best_buy()` / `best_sell()` — best price per side (returns `0` when that side
  is empty); `count_buy()` / `count_sell()` — order counts per side.
- `volume_buy(price)` / `volume_sell(price)` — aggregate resting qty at a level.

Not provided natively: no IOC / FOK / POST-ONLY; no native modify; matching has
no trade/fill callback that carries price+quantity (the `did_fill` callback
passes only a maker uid — see the source patch); no reject events; the engine's
`Price` is `uint64_t` and treats price `0` as the market-order sentinel.

## Adapter strategy

- **Trades** are surfaced by the injected per-fill hook (see "Source patch"):
  the hook fires once per fill with the maker uid, the maker's resting price,
  and the fill quantity, and the adapter emits one `ME_TRADE` per call (maker
  price mapped back to harness ticks; the aggressor's `sequence_number` and
  order id threaded through a per-call context set just before the `limit()`
  call). The hook also accumulates the taker's filled tally (drives the IOC
  residual) and decrements the maker's liveness shadow.
- **Prices**: the engine reserves `0` for market orders and the harness's signed
  `int64` ticks can be `≤ 0`, so every price is shifted into a strictly-positive
  engine space with a fixed additive offset `PX_OFF = 2^30` on the way in and
  subtracted on the way out (best bid/ask, depth-at, trade maker price). The
  canonical workload's prices sit around the NVDA reference (~33.5k ticks) and
  never approach `2^30`, so the map is a strictly-increasing bijection that
  preserves every price comparison.
- **IOC**: the engine has no IOC flag, so the order is submitted as a plain
  limit, matches what it can, and the adapter then cancels the rested residual
  (via the engine's native `cancel`) and emits one `CancelAck` for it, so an IOC
  order never rests.
- **Modify**: no native modify — explicit `cancel` + re-submit at the new
  price/quantity (the reinsert loses queue priority; its crossing fills carry
  the modify message's `sequence_number`, per the harness contract).
- A per-order **liveness shadow** (a flat array indexed by the dense harness
  order id; grow-only, pre-sized to 2^22 entries) holds `{price, side,
  remaining, alive}`. It is required to adjudicate cancel/modify of a
  not-resting order (already filled / already cancelled / never seen →
  `CancelReject` / `ModifyReject`; the canonical workload injects ~2% stale
  cancels/modifies) and to echo the resting order's side/price on the ack (the
  engine surfaces neither on cancel). It is **not** consulted for the audit
  queries — `engine_query_best_bid/ask` and `engine_query_depth_at` read the
  engine's live book directly (`best_buy`/`best_sell`/`count_*`/`volume_*`), so a
  stale shadow can never fool the state audit.

`OrderAck` / `CancelAck` / `ModifyAck` / `CancelReject` / `ModifyReject` are
synthesised above the engine, which has no ack/reject callback. The matcher runs
synchronously, so `engine_flush` is a no-op and no `engine_prebuild` is exported
(there is no translation-only prebuild step; all work happens in the
`engine_on_*` handlers).

## Source patch

`build.sh` applies **one** source patch to the engine, after `git reset --hard`
to the pin so the reset can never clobber it (idempotent; fails loud if its
anchor drifts):

- **Per-fill trade hook** (adapter instrumentation, `include/limit_tree.hpp`).
  The match loop `LimitTree::market` reports a consumed maker to its `did_fill`
  callback **by uid only** — it carries no per-fill price or quantity — and for
  the *last* maker, when that maker is only *partially* consumed, it does not
  invoke the callback at all. The harness needs one `ME_TRADE` per fill carrying
  the maker's resting price and the fill quantity, in match order. The patch
  forward-declares `extern "C" void __kautenja_trade_hook(maker_uid,
  maker_price, fill_qty)` and inserts one call to it at each of `market`'s two
  fill sites. It adds only those two emit points; the matching logic, prices,
  and quantities are otherwise byte-identical to the pinned source. The adapter
  implements the hook. This is the same "a hook the engine should call but
  doesn't" pattern used by `jxm35_adapter`.

This patch is **not** the engine's duplicate-id correctness fix. The harness
classifies Kautenja "with fix" for a separate, real engine defect filed upstream
as
[Kautenja/limit-order-book#4](https://github.com/Kautenja/limit-order-book/issues/4):
`limit_buy`/`limit_sell` call `orders.emplace(uid, …)` unchecked, so submitting
an order whose `uid` already names a *live* resting order silently keeps the
first order and then re-inserts it into the side's FIFO — corrupting the list
(self-linked node), double-counting that level's volume, and leaving a
use-after-free for the next cancel of that uid. The canonical benchmark workload
assigns a fresh dense id to every new order, so this path is never exercised and
the audit is `VALID` against the unmodified insert logic; `build.sh` therefore
does not carry a duplicate-id guard. The fix (reject — or treat as an amend — a
duplicate live id, rather than `emplace` into a slot that is already occupied)
is the one filed upstream in issue #4.

## Build / run

```bash
bash additional_references/kautenja_adapter/build.sh
./harness --engine kautenja_adapter.so --scenario normal --mode audit \
          --matcher-core 52 --drainer-core 53
```

`build.sh` clones the engine into `third_party/kautenja_limit_order_book/` at the
pinned commit, initialises the three header-only submodules it needs
(`binary-search-tree`, `doubly-linked-list`, `robin-map` — the heavier test-only
submodules are skipped), applies the trade-hook patch, and compiles the
header-only engine + this adapter into `kautenja_adapter.so` at the repo root
with the system `g++` (C++20). Override: `ME_KAUTENJA_SRC=/path/to/existing/clone`
uses an existing checkout in place of cloning (the patch is re-applied
idempotently and the submodule init is skipped when the vendor headers are
already present).
