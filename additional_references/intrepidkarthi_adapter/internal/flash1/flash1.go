// Package flash1 drives the matching engine the way the flash1 benchmark harness
// does, and produces the harness's report stream (docs/FLASH1.md). It is the
// adapter's logic in plain Go, so it can be tested here; cmd/flash1engine is the
// cgo glue that exposes it through the harness's C ABI.
package flash1

import (
	"math"

	"github.com/intrepidkarthi/orderbook/pkg/matching"
	"github.com/intrepidkarthi/orderbook/pkg/types"
)

// Report types, as the harness numbers them (me_report_type_t).
const (
	OrderAck     = 0
	Trade        = 1
	CancelAck    = 2
	ModifyAck    = 3
	CancelReject = 4
	ModifyReject = 5
)

// Report is one entry of the harness's report stream (me_report_t).
type Report struct {
	Type    uint8
	Side    uint8
	Seq     uint64
	OrderID uint64
	Price   int64
	Qty     uint32
	Maker   uint64
	Taker   uint64
}

const (
	symbol = "FLASH1"
	// user is every order's owner: the workload has no accounts, which is also why
	// self-trade prevention is ALLOW.
	user = "u"
)

// Adapter is one book driven by harness messages. Not safe for concurrent use: the
// harness has one matcher thread, and so does the engine.
type Adapter struct {
	e        *matching.Engine
	emit     func(Report)
	evs      []matching.Event
	buf      []types.Trade
	toEngine map[uint64]int64 // client order id -> engine order id, while resting
	toClient map[int64]uint64 // engine order id -> client order id
}

// sink collects the engine's events for the command in progress.
type sink struct{ a *Adapter }

func (s sink) OnEvents(evs []matching.Event) { s.a.evs = append(s.a.evs, evs...) }

// New builds an adapter whose reports go to emit, in order.
func New(emit func(Report)) *Adapter {
	a := &Adapter{
		emit:     emit,
		toEngine: make(map[uint64]int64, 1<<16),
		toClient: make(map[int64]uint64, 1<<16),
	}
	cfg := matching.DefaultConfig(symbol)
	cfg.SelfTradePrevention = matching.STPAllow
	cfg.MaxOrders = 1 << 22
	cfg.EventSink = sink{a}
	a.e = matching.NewEngine(cfg)
	return a
}

func side(s uint8) types.Side {
	if s == 1 {
		return types.SideSell
	}
	return types.SideBuy
}

func sideByte(s types.Side) uint8 {
	if s == types.SideSell {
		return 1
	}
	return 0
}

// trades emits a Trade report for every fill in the collected events, naming orders
// by the client's ids, and returns the quantity the taker filled.
func (a *Adapter) trades(seq, taker uint64) uint32 {
	var filled uint32
	for _, ev := range a.evs {
		if ev.Kind != matching.EventTrade || ev.Trade == nil {
			continue
		}
		t := ev.Trade
		maker := a.toClient[t.MakerOrderID]
		a.emit(Report{Type: Trade, Seq: seq, OrderID: maker, Price: t.Price, Qty: uint32(t.Quantity),
			Maker: maker, Taker: taker})
		filled += uint32(t.Quantity)
	}
	return filled
}

// NewOrder handles a new limit order: OrderAck, one Trade per fill, and for an IOC
// order a CancelAck for whatever did not fill.
func (a *Adapter) NewOrder(seq, oid uint64, price int64, qty uint32, s uint8, ioc bool) {
	a.emit(Report{Type: OrderAck, Side: s, Seq: seq, OrderID: oid, Price: price, Qty: qty})
	tif := types.TIFGoodTillCancel
	if ioc {
		tif = types.TIFImmediateOrCancel
	}
	o, err := types.NewOrder(user, symbol, side(s), types.OrderTypeLimit, price, int64(qty), tif)
	if err != nil {
		return
	}
	a.evs = a.evs[:0]
	a.buf, _, _ = a.e.Match(o, a.buf[:0])
	a.toClient[o.ID] = oid
	filled := a.trades(seq, oid)
	if ioc {
		if filled < qty {
			a.emit(Report{Type: CancelAck, Side: s, Seq: seq, OrderID: oid, Price: price, Qty: qty - filled})
		}
		return
	}
	if filled < qty {
		a.toEngine[oid] = o.ID
	}
}

// Cancel removes a resting order (CancelAck with its side, price and remaining
// quantity) or rejects one that is not resting, whether filled, cancelled or never
// seen (CancelReject).
func (a *Adapter) Cancel(seq, oid uint64) {
	eid, ok := a.toEngine[oid]
	if !ok {
		a.emit(Report{Type: CancelReject, Seq: seq, OrderID: oid})
		return
	}
	o, err := a.e.Cancel(eid, user)
	if err != nil {
		// It filled since it rested: the map entry is stale, and the engine knows.
		delete(a.toEngine, oid)
		a.emit(Report{Type: CancelReject, Seq: seq, OrderID: oid})
		return
	}
	delete(a.toEngine, oid)
	a.emit(Report{Type: CancelAck, Side: sideByte(o.Side), Seq: seq, OrderID: oid, Price: o.Price, Qty: uint32(o.RemainingQty)})
}

// Modify is cancel and re-add, losing priority: ModifyAck with the new price and
// quantity, then the re-added order's trades under the modify's sequence number. An
// order that is not resting gets ModifyReject.
func (a *Adapter) Modify(seq, oid uint64, price int64, qty uint32, s uint8) {
	eid, ok := a.toEngine[oid]
	if !ok {
		a.emit(Report{Type: ModifyReject, Seq: seq, OrderID: oid})
		return
	}
	o, err := types.NewOrder(user, symbol, side(s), types.OrderTypeLimit, price, int64(qty), types.TIFGoodTillCancel)
	if err != nil {
		a.emit(Report{Type: ModifyReject, Seq: seq, OrderID: oid})
		return
	}
	a.evs = a.evs[:0]
	res, err := a.e.Replace(eid, user, o)
	if err != nil {
		delete(a.toEngine, oid)
		a.emit(Report{Type: ModifyReject, Seq: seq, OrderID: oid})
		return
	}
	delete(a.toEngine, oid)
	a.toClient[res.Order.ID] = oid
	a.emit(Report{Type: ModifyAck, Side: s, Seq: seq, OrderID: oid, Price: price, Qty: qty})
	if filled := a.trades(seq, oid); filled < qty {
		a.toEngine[oid] = res.Order.ID
	}
}

// BestBid is the highest resting bid, or math.MinInt64 for an empty side.
func (a *Adapter) BestBid() int64 {
	if p, _, ok := a.e.BestBid(); ok {
		return p
	}
	return math.MinInt64
}

// BestAsk is the lowest resting ask, or math.MaxInt64 for an empty side.
func (a *Adapter) BestAsk() int64 {
	if p, _, ok := a.e.BestAsk(); ok {
		return p
	}
	return math.MaxInt64
}

// DepthAt is the resting quantity at one price on one side, 0 if none. Audit queries
// are rare, so summing the level's orders is fine.
func (a *Adapter) DepthAt(price int64, s uint8) uint64 {
	var total uint64
	for _, o := range a.e.Book().GetOrdersAtPrice(side(s), price) {
		total += uint64(o.RemainingQty)
	}
	return total
}
