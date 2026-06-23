package exchange

// me_shim.go — exported shim over the unexported go-trader matcher so the
// benchmark cgo adapter (cmd/meadapter, same module) can drive it directly.
//
// This file is ADDED by the adapter build step; it is not part of upstream
// go-trader. It exposes the in-process limit-order book (orderBook.add /
// orderBook.remove / matchTrades, all unexported) without standing up the
// FIX/gRPC exchange runtime. It carries no matching logic of its own — every
// cross, fill, and removal is the engine's. See cmd/meadapter/wrapper.go.
//
// Prices and quantities cross the C ABI as integers; the shim maps them to the
// engine's robaho/fixed Fixed decimal with NewI(v,0) (== Fixed{fp:v*10^7}),
// which is a strictly-increasing, exactly-invertible mapping (Int() recovers v),
// so price ordering and equality inside the book are bit-identical to integers.

import (
	"time"

	. "github.com/robaho/fixed"

	. "github.com/robaho/go-trader/pkg/common"
)

// MeFill is one fill, flattened from the engine's trade for the C boundary.
type MeFill struct {
	MakerID    int64
	TakerID    int64
	PriceTicks int64
	Qty        uint32
}

// MeBook is a single drivable order book plus the resting-order handles the
// cancel/modify path needs. The engine keys orderList.allOrders by the full
// sessionOrder value, so a cancel must present the exact sessionOrder that was
// added — we persist it here per order id.
type MeBook struct {
	ob   orderBook
	live map[OrderID]sessionOrder
	clk  time.Time // monotonic per-order timestamp source (see MeAdd)
	// fills is a single reused scratch slice for MeAdd's flattened fills. The
	// matcher thread is single-threaded (harness contract) and each call site
	// fully consumes the returned slice (emits its trades) before the next
	// MeAdd, so one book-owned buffer reset by length avoids a per-message
	// make() on the timed hot path. Mirrors the sibling Go adapters' reused
	// capture slice (e.g. fmstephe gCapture.msgs[:0]).
	fills []MeFill
}

// meClient is a single shared no-op exchangeClient. The book itself never
// invokes client methods (only the exchange wrapper does); it is present only
// because sessionOrder carries it and uses it in the allOrders map key, so it
// must be the SAME value for every order for map-key equality to hold.
type meClient struct{}

func (meClient) SendOrderStatus(sessionOrder) {}
func (meClient) SendTrades([]trade)           {}
func (meClient) SessionID() string            { return "ME" }

var theMeClient meClient

// NewMeBook returns an empty order book for one symbol.
func NewMeBook() *MeBook {
	return &MeBook{
		live:  make(map[OrderID]sessionOrder, 1<<21),
		clk:   time.Unix(0, 0),
		fills: make([]MeFill, 0, 1024),
	}
}

// nextTime hands out strictly increasing, unique timestamps. The matcher's
// maker-price rule (orderbook.go: the resting order is the one with the
// earlier sessionOrder.time) needs the aggressor to be strictly newer than
// every resting order; raw time.Now() can repeat at clock resolution, so we
// drive a deterministic monotone clock instead.
func (b *MeBook) nextTime() time.Time {
	b.clk = b.clk.Add(time.Nanosecond)
	return b.clk
}

// MeAdd submits a limit order, runs matching, and returns the fills (in match
// order) plus the resting remainder (0 if fully filled / not resting). It is a
// faithful call into orderBook.add — the engine does all crossing.
func (b *MeBook) MeAdd(oid int64, side uint8, priceTicks int64, qty uint32, ioc bool) ([]MeFill, uint32) {
	var s Side
	if side == 0 {
		s = Buy
	} else {
		s = Sell
	}
	order := LimitOrder(b.ob.Instrument, s, NewI(priceTicks, 0), NewI(int64(qty), 0))
	order.Id = OrderID(oid)

	so := sessionOrder{theMeClient, order, b.nextTime()}

	trades, _ := b.ob.add(so)

	// Reuse the book-owned scratch slice (reset by length, no per-message
	// make()); the caller fully consumes it before the next MeAdd.
	fills := b.fills[:0]
	for i := range trades {
		t := &trades[i]
		var makerID, takerID int64
		// The aggressor is the order we just added (this side). The maker is
		// the opposite, resting side of each trade.
		if s == Buy {
			takerID = int64(t.buyer.order.Id)
			makerID = int64(t.seller.order.Id)
		} else {
			takerID = int64(t.seller.order.Id)
			makerID = int64(t.buyer.order.Id)
		}
		fills = append(fills, MeFill{
			MakerID:    makerID,
			TakerID:    takerID,
			PriceTicks: t.price.Int(),
			Qty:        uint32(t.quantity.Int()),
		})
	}
	b.fills = fills // retain any grown backing array for reuse

	// Resting remainder: the order rests iff still active with quantity left.
	var residual uint32
	if order.IsActive() && order.Remaining.GreaterThan(ZERO) {
		residual = uint32(order.Remaining.Int())
	}

	if ioc {
		// IOC: any unfilled remainder must not rest. add() rests a limit
		// order's residual, so remove it explicitly (the engine's own remove).
		if residual > 0 {
			b.ob.remove(so)
		}
		return fills, residual
	}

	if residual > 0 {
		b.live[OrderID(oid)] = so
	}
	return fills, residual
}

// MeIsLive reports whether order id is currently tracked as resting.
func (b *MeBook) MeIsLive(oid int64) bool {
	_, ok := b.live[OrderID(oid)]
	return ok
}

// MeIsActive reports whether a tracked order is still in a live (non-terminal)
// engine state. The live *Order pointer is the same one matchTrades mutates as
// it fills, so once a later aggressor fully consumes a tracked order its
// IsActive() (orders.go: false for Filled / Cancelled / Rejected) flips to
// false even though our live map still holds the handle (we map a fill's
// residual, not its terminal state). Both the cancel and modify paths consult
// this to reject a stale request against an already-filled order.
func (b *MeBook) MeIsActive(oid int64) bool {
	so, ok := b.live[OrderID(oid)]
	if !ok {
		return false
	}
	return so.order.IsActive()
}

// MeRestingSide / MeRestingPrice / MeRestingQty echo the resting order's fields
// (the cancel/modify report wire format requires the order's side and price).
func (b *MeBook) MeRestingSide(oid int64) uint8 {
	so := b.live[OrderID(oid)]
	if so.order.Side == Buy {
		return 0
	}
	return 1
}
func (b *MeBook) MeRestingPrice(oid int64) int64 {
	return b.live[OrderID(oid)].order.Price.Int()
}
func (b *MeBook) MeRestingQty(oid int64) uint32 {
	so := b.live[OrderID(oid)]
	if so.order == nil {
		return 0
	}
	return uint32(so.order.Remaining.Int())
}

// MeCancel removes a resting order via the engine's own remove(), mirroring the
// engine's native exchange.CancelOrder result. Returns false (caller emits a
// CancelReject) when the engine's CancelOrder would reject — i.e. there is no
// such id, or the id is no longer cancellable.
//
// The stale-cancel case: an order can still be in our live map after a later
// aggressor FULLY FILLED it (we map a fill's residual, not its terminal state).
// The engine's CancelOrder, called on a filled order, rebuilds a fresh-time
// sessionOrder and hands it to orderBook.remove, which returns OrderNotFound
// (the order was already removed from its price level as it filled, and the
// allOrders map is keyed by the full sessionOrder value, so the fresh-time key
// never matches). We reproduce that reject without standing up the exchange by
// gating on the order's own IsActive() (orders.go: false for Filled / Cancelled
// / Rejected) — the live *Order pointer is the same one matchTrades mutated to
// Filled, so a stale cancel of a fully-filled order rejects, exactly as native.
func (b *MeBook) MeCancel(oid int64) bool {
	so, ok := b.live[OrderID(oid)]
	if !ok {
		return false
	}
	if !so.order.IsActive() {
		// Fully filled (or otherwise terminal) while still tracked here: the
		// engine's CancelOrder would return OrderNotFound. Drop the stale handle
		// and reject.
		delete(b.live, OrderID(oid))
		return false
	}
	delete(b.live, OrderID(oid))
	b.ob.remove(so)
	return true
}

// MeBestBid returns the highest bid price in ticks, or false if no bids.
func (b *MeBook) MeBestBid() (int64, bool) {
	if len(b.ob.bids) == 0 {
		return 0, false
	}
	return b.ob.bids[0].price.Int(), true
}

// MeBestAsk returns the lowest ask price in ticks, or false if no asks.
func (b *MeBook) MeBestAsk() (int64, bool) {
	if len(b.ob.asks) == 0 {
		return 0, false
	}
	return b.ob.asks[0].price.Int(), true
}

// MeDepthAt returns the aggregated resting quantity at one price level, read
// straight from the live engine book (not a shadow) so the anti-cheat audit
// measures the real structure.
func (b *MeBook) MeDepthAt(priceTicks int64, side uint8) uint64 {
	target := NewI(priceTicks, 0)
	var levels []priceLevel
	if side == 0 {
		levels = b.ob.bids
	} else {
		levels = b.ob.asks
	}
	for i := range levels {
		if levels[i].price.Equal(target) {
			var total uint64
			for node := levels[i].head; node != nil; node = node.next {
				total += uint64(node.order.order.Remaining.Int())
			}
			return total
		}
	}
	return 0
}
