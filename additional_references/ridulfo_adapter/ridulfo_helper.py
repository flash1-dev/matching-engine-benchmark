"""
ridulfo_helper.py — thin in-process driver around the ridulfo engine
(ordermatchinengine.Orderbook) for the matching-engine benchmark adapter.

The C++ adapter (ridulfo_adapter.cpp) embeds CPython, imports this module once,
and calls the four entry points below per message. All matching is done by the
ENGINE (Orderbook.process_order); this module only marshals arguments, reads the
engine's own trade list / book state back out, and enforces the two things the
engine's API genuinely cannot express:

  * IOC residual: ridulfo has no IOC order type. We submit an IOC as a native
    LimitOrder (so it crosses exactly where the engine's own price logic lets
    it), then, if a residual rested, remove that residual from the engine's book
    and report its quantity so the adapter can emit the CancelAck. No matching
    is reimplemented — only the "do not rest the remainder" rule is applied.

  * cancel / modify liveness: process_order(CancelOrder) silently no-ops whether
    or not the order was resting, and there is no modify type at all. We read
    liveness DIRECTLY from the engine's own book (Orderbook.bids / .asks), so the
    reject decision is the engine's true state, not a separate shadow.

FIFO time priority: Order.time is normally int(1e6*time()) (wall-clock micro-
seconds). We instead stamp each order with a strictly increasing arrival counter
so the run is deterministic AND the engine gets proper first-in-first-out tie
breaking among equal-price orders (its intended arrival-time semantics).

Source patch (applied by build.sh): LimitOrder.__lt__ is corrected to a
consistent total order — a stable final tiebreak on the unique order_id —
because the upstream comparator fell to a size compare (a smaller order jumps
ahead of an older equal-price one) and returned None when price, time, and size
all tied, which silently breaks SortedList.discard() so a cancel can no longer
find its order. See ridulfo/order-matching-engine#10. The arrival counter above
already keeps Order.time unique per order in this driver, so the fix is not what
makes the canonical run pass here — but it is the documented engine correctness
fix and is load-bearing the moment two equal-price orders share a time (e.g. the
engine's own wall-clock microsecond stamping at its ~400k orders/s throughput).
"""

from ordermatchinengine import Orderbook, LimitOrder, MarketOrder, CancelOrder, Side

_BUY = Side.BUY
_SELL = Side.SELL

book = Orderbook()


def reset():
    global book
    book = Orderbook()


def _side(s):
    return _BUY if s == 0 else _SELL


def _find_resting(oid):
    """Return the resting Order with this id (searching the engine's own book),
    or None. Liveness is the engine's book state, exactly what a cancel/modify
    must adjudicate against."""
    for o in book.bids:
        if o.order_id == oid:
            return o
    for o in book.asks:
        if o.order_id == oid:
            return o
    return None


def submit_limit(oid, side, qty, price, arrival, ioc):
    """Submit a (possibly IOC) limit order. Returns
       (trades, residual_qty)
    where trades is a list of (price_ticks, qty, maker_id) in match order and
    residual_qty is the IOC unfilled remainder that was pulled back out of the
    book (0 for a non-IOC order, or an IOC that fully filled / rested nothing)."""
    n0 = len(book.trades)
    order = LimitOrder(oid, _side(side), qty, price)
    order.time = arrival
    book.process_order(order)
    trades = [(t.price, t.size, t.book_order_id) for t in book.trades[n0:]]

    residual = 0
    if ioc:
        # The engine rests an unfilled limit remainder; an IOC must not rest it.
        # order.remaining is the unfilled quantity; if >0 the engine added the
        # live `order` object to bids/asks (process_order rests `incoming_order`
        # itself), so discard that exact object.
        if order.remaining > 0:
            residual = order.remaining
            if order.side == _BUY:
                book.bids.discard(order)
            else:
                book.asks.discard(order)
    return trades, residual


def submit_market(oid, side, qty, arrival):
    """Submit a market order (matches across the book at any price, residual
    dropped — the engine never rests a MarketOrder). Returns (trades, residual)
    where residual is the unfilled quantity (reported as an IOC-style CancelAck
    by the adapter, since a market order is immediate-or-cancel by nature)."""
    n0 = len(book.trades)
    order = MarketOrder(oid, _side(side), qty)
    order.time = arrival
    book.process_order(order)
    trades = [(t.price, t.size, t.book_order_id) for t in book.trades[n0:]]
    residual = order.remaining
    return trades, residual


def cancel(oid):
    """Cancel by id. Returns (resting, side, price): resting True iff the order
    was resting (the engine removed it), else (False, 0, 0) -> CancelReject. The
    side/price are the cancelled order's, echoed in the CancelAck. Liveness and
    the echo both come from the engine's own book state."""
    o = _find_resting(oid)
    if o is None:
        return (False, 0, 0)
    side = 0 if o.side == _BUY else 1
    price = o.price
    book.process_order(CancelOrder(oid))
    return (True, side, price)


def modify(oid, side, new_qty, new_price, arrival):
    """Modify = cancel + reinsert. Returns (ok, trades): ok False -> ModifyReject
    (order not resting). On ok the order is removed and re-added at the new
    price/qty (losing queue priority), crossing through the engine; trades are
    the fills the reinsert produced."""
    o = _find_resting(oid)
    if o is None:
        return False, []
    book.process_order(CancelOrder(oid))
    n0 = len(book.trades)
    order = LimitOrder(oid, _side(side), new_qty, new_price)
    order.time = arrival
    book.process_order(order)
    trades = [(t.price, t.size, t.book_order_id) for t in book.trades[n0:]]
    return True, trades


def best_bid():
    b = book.get_bid()
    return b if b is not None else None


def best_ask():
    a = book.get_ask()
    return a if a is not None else None


def depth_at(price, side):
    """Aggregated resting quantity at one price level on `side`."""
    total = 0
    book_side = book.bids if side == 0 else book.asks
    for o in book_side:
        if o.price == price:
            total += o.remaining
    return total
