"""
pyme_driver.py — thin orchestration layer between the matching-engine benchmark
C ABI (driven from pyme_adapter.cpp via the CPython C-API) and the upstream
Surbeivol/PythonMatchingEngine `Orderbook` class.

This module does NOT reimplement matching. It calls the engine's NATIVE API:

  * Orderbook.send(...)   — submit a new order; the engine matches it against the
                            opposite side and records each resulting fill into
                            ob.trades (price = MAKER resting price, agg_ord =
                            taker uid, pas_ord = maker uid), in match order.
  * Orderbook.cancel(uid) — native cancel-by-id of a resting order.
  * ob._orders[uid]       — the engine's own per-order table (uid -> Order), used
                            as the authoritative "is this order resting?" oracle
                            (Order.active) and to echo a resting order's side /
                            price on a CancelAck (the engine surfaces both on the
                            Order object, so no separate adapter shadow is kept).

Harness conventions implemented here (see docs/INTEGRATION.md):
  * Trade.price_ticks = the maker's (resting) price; Trade.sequence_number = the
    aggressor's; maker_order_id / taker_order_id are the two orders; fills are
    emitted in match order.
  * IOC = submit as a normal limit, let it match, then cancel the rested
    residual and emit ONE CancelAck for it (drop, do not rest).
  * modify = cancel + reinsert at the new price/qty (losing time priority); the
    reinsert may itself cross and produce Trades; emit one ModifyAck, or a
    ModifyReject if the order is not resting.
  * CancelReject / ModifyReject for a cancel/modify of an order that is not
    resting (already filled, already cancelled, or never seen).

Prices: the harness uses signed integer ticks. pyme keys its book by float
price. We feed the engine `float(price_ticks)` (integer-valued, exact in IEEE
double for the workload's range) and convert the maker price back with
`int(round(px))`, which is exact. is_mine=True is passed on every send so the
engine's historical-order market-impact path is bypassed (it only rewrites
prices for is_mine=False), keeping prices pristine.

Report tuple layout (consumed by the C++ side):
  (rtype, side, seq, order_id, price_ticks, quantity, maker_id, taker_id)
rtype: 0 OrderAck, 1 Trade, 2 CancelAck, 3 ModifyAck, 4 CancelReject, 5 ModifyReject
"""

import warnings
warnings.filterwarnings("ignore")

from marketsimulator.orderbook import Orderbook

# Report type codes (mirror me_report_type_t).
ORDER_ACK = 0
TRADE = 1
CANCEL_ACK = 2
MODIFY_ACK = 3
CANCEL_REJECT = 4
MODIFY_REJECT = 5

_ob = None          # the single Orderbook instance
_orders = None      # alias of _ob._orders (uid -> Order), the native order table


def engine_init(seed):
    """Create one empty order book. `seed` is unused (the workload is fixed by
    the harness); accepted to mirror the ABI."""
    global _ob, _orders
    # band6 = highest-liquidity tick regime; the ticker name is cosmetic here.
    # resilience=0 makes the market-impact accumulator inert even if it were
    # consulted (it is not, because every send below is is_mine=True).
    _ob = Orderbook(ticker="band6stock", resilience=0)
    _orders = _ob._orders
    return 0


def engine_shutdown():
    global _ob, _orders
    _ob = None
    _orders = None
    return 0


def _is_resting(uid):
    o = _orders.get(uid)
    return o is not None and o.active


def on_new_order(order_id, seq, price_ticks, quantity, side, ioc):
    """side: 0 buy, 1 sell. ioc: 0/1. Returns list of report tuples."""
    is_buy = (side == 0)
    px = float(price_ticks)
    reports = []

    # OrderAck first — the engine has accepted the new order.
    reports.append((ORDER_ACK, side, seq, order_id, price_ticks, quantity, 0, 0))

    n0 = _ob.ntrds
    _ob.send(is_buy=is_buy, qty=quantity, price=px, uid=order_id, is_mine=True)
    n1 = _ob.ntrds

    # One Trade per fill, in match order. ob.trades columns:
    #   'price'  = maker (resting) price, 'vol' = fill qty,
    #   'agg_ord' = taker uid (== order_id), 'pas_ord' = maker uid.
    filled = 0
    if n1 > n0:
        t = _ob.trades
        tpx = t["price"]
        tvol = t["vol"]
        tmaker = t["pas_ord"]
        for i in range(n0, n1):
            q = int(tvol[i])
            filled += q
            reports.append((TRADE, side, seq, order_id,
                            int(round(tpx[i])), q,
                            int(tmaker[i]), order_id))

    residual = quantity - filled
    if ioc:
        # IOC: the engine rested any residual; remove it and emit ONE CancelAck.
        if residual > 0:
            _ob.cancel(order_id)
            reports.append((CANCEL_ACK, side, seq, order_id,
                            price_ticks, residual, 0, 0))
        # residual == 0: fully filled, nothing rests, no extra report.
    # GTC: residual (if any) already rests inside the engine; its liveness is
    # tracked by _orders[order_id].active. No bookkeeping needed here.
    return reports


def on_cancel(order_id, seq):
    if _is_resting(order_id):
        o = _orders[order_id]
        side = 0 if o.is_buy else 1
        price_ticks = int(round(o.price))
        _ob.cancel(order_id)
        return [(CANCEL_ACK, side, seq, order_id, price_ticks, 0, 0, 0)]
    # Not resting — already filled, already cancelled, or never seen.
    return [(CANCEL_REJECT, 0, seq, order_id, 0, 0, 0, 0)]


def on_modify(order_id, seq, new_price_ticks, new_quantity, side):
    """modify = cancel + reinsert at the new price/qty. The order's side is
    given by the harness (modify_t.side)."""
    if not _is_resting(order_id):
        return [(MODIFY_REJECT, 0, seq, order_id, 0, 0, 0, 0)]

    is_buy = (side == 0)
    px = float(new_price_ticks)
    reports = []

    # Cancel the resting order, then reinsert fresh (loses time priority).
    _ob.cancel(order_id)

    n0 = _ob.ntrds
    _ob.send(is_buy=is_buy, qty=new_quantity, price=px, uid=order_id,
             is_mine=True)
    n1 = _ob.ntrds

    if n1 > n0:
        t = _ob.trades
        tpx = t["price"]
        tvol = t["vol"]
        tmaker = t["pas_ord"]
        for i in range(n0, n1):
            reports.append((TRADE, side, seq, order_id,
                            int(round(tpx[i])), int(tvol[i]),
                            int(tmaker[i]), order_id))

    # Exactly one ModifyAck (whatever the reinsert filled / rested).
    reports.append((MODIFY_ACK, side, seq, order_id,
                    new_price_ticks, new_quantity, 0, 0))
    return reports


def query_best_bid():
    bb = _ob.best_bid
    return None if bb is None else int(round(bb[0]))


def query_best_ask():
    ba = _ob.best_ask
    return None if ba is None else int(round(ba[0]))


def query_depth_at(price_ticks, side):
    book = _ob._bids.book if side == 0 else _ob._asks.book
    pl = book.get(float(price_ticks))
    return 0 if pl is None else int(pl.vol)
