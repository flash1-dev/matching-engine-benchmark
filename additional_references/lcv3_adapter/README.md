# lcv3_adapter — integration example

Wraps [lc-lukaszczerwinski/public](https://github.com/lc-lukaszczerwinski/public)
(LC++ v3) behind `api/matching_engine_api.h`.

Pinned commit:
- `lc-lukaszczerwinski/public` — `f956a09190c41f8873438f9e3a0a386777a87647`

This adapter is one of the worked examples in `additional_references/` —
none are baselines and none are maintained. See `CORRECTNESS_FINDINGS.md` at the
repository root for the observations the harness produced against this
snapshot.

## Engine shape

Header-only C++ limit-order book. The engine is a class template
`v3::OrderBook<TEventHandler>`, instantiated with an event-handler type.
All matching runs synchronously on the caller thread; `engine_flush()` is a
no-op.

Native APIs visible from the adapter:

- `OrderBook<EventHandler>(events)` — constructed with the event-handler
  reference; allocates fixed pools for orders (`OrderFixedPool`) and price
  levels.
- `OrderBook::insert_order<side>(id, price, qty, seq)` — submits a GTC order;
  matches first, then rests any residual. Returns the pool slot index (used
  for later cancel/modify), or `-1` if fully filled. Fires `orderACK` then
  zero or more `trade` callbacks.
- `OrderBook::insert_ioc_order<side>(id, price, qty, seq)` — submits an IOC
  order; matches, then fires `cancelACK` for any unfilled residual
  (never rested).
- `OrderBook::cancel_order<side>(id, price, slot, seq)` — cancels by pool
  slot; fires `cancelACK` (with remaining quantity) on success or
  `cancelReject` if the slot is stale (wrong id or price).
- `OrderBook::update_order<side>(id, oldPrice, newPrice, newQty, slot, seq)`
  — atomic cancel + reinsert; fires `modifyACK` then crossing `trade`
  callbacks for the reinsertion, or `modifyReject` if the slot is stale.
- `OrderBook::best_bid()` / `best_ask()` — O(1) treap-front lookup.
- `OrderBook::_ext_depth_at<side>(price)` — total resting quantity at a price
  level (linear scan of the level's intrusive FIFO queue).

Event callbacks on `TEventHandler` (all synchronous, fired from within
`insert_*` / `cancel_order` / `update_order`):

- `orderACK(seq, id, side, price, qty)`
- `trade(seq, maker, taker, price, qty)` — maker and taker order ids, maker's
  resting price, fill quantity.
- `cancelACK(seq, id, side, price, qty)` — remaining quantity at time of cancel.
- `cancelReject(seq, id)`
- `modifyACK(seq, id, side, newPrice, newQty)` — fired before the reinsert.
- `modifyReject(seq, id)`

Side encoding: engine uses `Buy = 1`, `Sell = -1`; harness uses `0 = buy`,
`1 = sell`. Conversion: `harness_side = (-engine_side + 1) / 2`.

## Adapter strategy

- An `EventHandler` struct with one method per engine event translates
  each callback directly into a harness report push. No synthesised acks —
  the engine supplies every field.
- Three flat arrays (`g_side`, `g_price`, `g_slot_id`), sized to
  `1 << 21` and indexed by order id, carry the per-order state the engine
  doesn't surface at cancel/modify time: the resting side and price (required
  by `cancel_order` / `update_order`) and the pool slot index.
- `g_price` is also refreshed inside `EventHandler::modifyACK` (fired by
  `update_order` before the reinsert) so that a subsequent modify on the same
  order passes the correct `oldPrice`.
- **IOC** is native — `insert_ioc_order` handles the residual `cancelACK`
  internally.
- **Modify** is native — `update_order` handles the cancel + reinsert and
  emits the `modifyACK` + crossing `trade`s.
- **`engine_query_*`** delegate directly to `best_bid()`, `best_ask()`, and
  `_ext_depth_at<side>()`.
- No source patch applied.

## Build / run

```bash
bash additional_references/lcv3_adapter/build.sh
./harness --engine lcv3_adapter.so --scenario normal --mode audit
```

`build.sh` clones lc-lukaszczerwinski/public into `third_party/lcv3/` at the
pinned commit, then compiles the adapter (header-only engine — no engine
`.cpp` files compiled in) into `lcv3_adapter.so` at the repo root.
Override `ME_LCV3_SRC` to point at an existing checkout instead of cloning.
