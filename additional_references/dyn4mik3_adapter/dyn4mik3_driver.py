"""
dyn4mik3_driver.py — thin translation layer between the harness ABI and the
dyn4mik3/OrderBook pure-Python matching engine.

ALL matching is the engine's: process_order (limit/market crossing),
cancel_order, and the engine's red-black OrderTree. This module only:
  * translates harness side codes (0=bid,1=ask) to the engine's 'bid'/'ask',
  * supplies the harness order_id / sequence_number to the engine as the
    order's order_id / timestamp (from_data=True, so the engine does NOT use
    its own auto-increment id and does NOT overwrite our id),
  * reads the engine's returned trade records into harness Trade tuples
    (maker resting price + maker/taker ids),
  * maps IOC = submit-limit-then-cancel-any-rested-remainder,
  * maps modify = cancel + reinsert via the engine's OWN cancel + process_order
    (the engine's native update_order does NOT cross the book on a reprice, so
    to honour the harness "Trade per crossing fill" modify contract we use the
    engine's cancel + its crossing process_order — both native; no matching is
    reimplemented here),
  * adjudicates cancel/modify rejects from the engine's authoritative
    order_exists() (its per-side order_map), NOT from any private shadow.

Report tuples returned to the C++ shim (one list per message), each a flat
tuple whose first element is the me_report_type_t value:
  OrderAck     (0, seq, side, order_id, price, qty)
  Trade        (1, seq, price, qty, maker_id, taker_id)
  CancelAck    (2, seq, side, order_id, price)
  ModifyAck    (3, seq, side, order_id, price, qty)
  CancelReject (4, seq, order_id)
  ModifyReject (5, seq, order_id)
The C++ side serialises these into the wire me_report_t; field order matches
correctness.cpp's canonical line format.
"""

from orderbook import OrderBook

# Side code (harness) -> engine side string.
_BID = 'bid'
_ASK = 'ask'


class Driver(object):
    def __init__(self):
        # tick_size is irrelevant to matching here (we feed integer-tick prices
        # straight through); the default is fine.
        self.ob = OrderBook()

    # ---- helpers -----------------------------------------------------------

    def _side_str(self, side):
        return _BID if side == 0 else _ASK

    def _resting_side(self, oid):
        """Return (side_code, Order) if oid is resting, else (None, None).

        The engine's per-side order_map is the single source of truth for
        liveness: a fully-filled or already-cancelled maker has been removed
        from it (ordertree.remove_order_by_id), so this rejects correctly for
        filled-away / cancelled / never-seen ids alike.
        """
        if self.ob.bids.order_exists(oid):
            return 0, self.ob.bids.get_order(oid)
        if self.ob.asks.order_exists(oid):
            return 1, self.ob.asks.get_order(oid)
        return None, None

    def _trades_to_reports(self, seq, taker_id, trades, out):
        # transaction_record['party1'] == [counter_party_trade_id, side,
        #   maker_order_id, new_book_quantity]; the resting (maker) order's
        # price is trade['price'], its id is party1[2]. taker is the incoming
        # order (party2 carries no id in the engine, so we pass it in).
        for tr in trades:
            maker_id = tr['party1'][2]
            out.append((1, seq, int(tr['price']), int(tr['quantity']),
                        int(maker_id), int(taker_id)))

    # ---- hot path ----------------------------------------------------------

    def on_new(self, oid, seq, price, qty, side, ioc):
        out = []
        # OrderAck for every accepted new order.
        out.append((0, seq, side, oid, price, qty))
        side_str = self._side_str(side)
        quote = {
            'type': 'limit',
            'side': side_str,
            'quantity': qty,
            'price': price,            # integer tick; engine wraps in Decimal
            'trade_id': oid,           # used only for the engine's own party labels
            'order_id': oid,           # honoured because from_data=True
            'timestamp': seq,          # honoured because from_data=True
        }
        # from_data=True: engine uses our order_id + timestamp verbatim and does
        # not touch its internal next_order_id. verbose=False.
        trades, order_in_book = self.ob.process_order(quote, True, False)
        self._trades_to_reports(seq, oid, trades, out)

        if ioc:
            # IOC: the engine has no native IOC, so we rest-then-cancel any
            # remainder. order_in_book is the residual quote dict when the
            # incoming order did not fully fill.
            if order_in_book is not None:
                residual_qty = int(order_in_book['quantity'])
                self.ob.cancel_order(side_str, oid)
                # CancelAck for the IOC residual: side + limit price of the
                # cancelled remainder (qty omitted from the canonical line).
                out.append((2, seq, side, oid, price))
        # GTC: nothing more — the residual (if any) rests under our oid.
        return out

    def on_cancel(self, oid, seq):
        out = []
        side, order = self._resting_side(oid)
        if order is None:
            # Not resting (filled / cancelled / never seen): CancelReject.
            out.append((4, seq, oid))
            return out
        price = int(order.price)
        self.ob.cancel_order(self._side_str(side), oid)
        out.append((2, seq, side, oid, price))
        return out

    def on_modify(self, oid, seq, new_price, new_qty, side):
        out = []
        rside, order = self._resting_side(oid)
        if order is None:
            out.append((5, seq, oid))
            return out
        # Cancel + reinsert (both engine-native), so the reinsert CROSSES the
        # book and loses time priority — the harness modify contract. The
        # order's true resting side is rside (the side the harness sends is the
        # order's side and agrees, but we use the engine's authoritative side).
        side_str = self._side_str(rside)
        self.ob.cancel_order(side_str, oid)
        quote = {
            'type': 'limit',
            'side': side_str,
            'quantity': new_qty,
            'price': new_price,
            'trade_id': oid,
            'order_id': oid,
            'timestamp': seq,
        }
        trades, order_in_book = self.ob.process_order(quote, True, False)
        self._trades_to_reports(seq, oid, trades, out)
        # Exactly one ModifyAck. price/qty in the ack are the modify's target;
        # the canonical ModifyAck line carries side, order_id, new price, new
        # qty. (Whether a residual rested or it fully crossed, the ack is one.)
        out.append((3, seq, rside, oid, new_price, new_qty))
        return out

    # ---- audit queries -----------------------------------------------------

    def best_bid(self):
        p = self.ob.get_best_bid()
        return None if p is None else int(p)

    def best_ask(self):
        p = self.ob.get_best_ask()
        return None if p is None else int(p)

    def depth_at(self, price, side):
        side_str = self._side_str(side)
        # get_volume_at_price wraps price in Decimal and returns the level
        # volume (0 if the level is absent).
        return int(self.ob.get_volume_at_price(side_str, price))


# A module-global instance the C++ shim creates once in engine_init.
_driver = None


def init():
    global _driver
    _driver = Driver()


def shutdown():
    global _driver
    _driver = None
