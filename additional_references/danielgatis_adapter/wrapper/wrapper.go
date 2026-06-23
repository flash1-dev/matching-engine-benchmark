// wrapper.go — danielgatis/go-orderbook behind the harness
// matching_engine_api.h ABI.
//
// danielgatis/go-orderbook (https://github.com/danielgatis/go-orderbook) is a
// pure-Go price-time-priority limit order book (WK Selph design): a
// red-black tree of price levels (emirpasic/gods) whose leaves are FIFO
// container/list queues, an id->*list.Element map for cancel, and
// shopspring/decimal prices/quantities. Order ids and trader ids are STRINGS.
//
// Engine shape visible from the adapter:
//   - ProcessLimitOrder(orderID, traderID, side, amount, price) ([]*Trade, error)
//       matches against the resting book and RESTS the residual; returns the
//       fills as a []*Trade (taker id, maker id, amount, maker price). It does
//       NOT use a callback.
//   - CancelOrder(orderID) *Order  — removes a resting order; returns the
//       removed *Order, or nil if no such order is resting (the reject signal).
//   - No IOC: submit a normal limit then cancel the unfilled remainder.
//   - No native modify: cancel + reinsert (the engine README itself says
//       "Updates will have to be handled with Cancel+Create").
//
// Mapping decisions:
//   - harness uint64 order_id  -> decimal string key (strconv.FormatUint).
//   - traderID = the order_id string, so every order is from a DISTINCT
//     trader. The engine has self-trade prevention (it skips a resting maker
//     whose traderID equals the aggressor's, order_book_limit.go:71); giving
//     each order a unique trader makes STP inert for cross-order matching,
//     which is the faithful single-book price-time mapping (an order never
//     matches itself).
//   - price ticks (int64) -> decimal.NewFromInt(ticks); recovered with
//     .IntPart(). quantity (uint32) -> decimal.NewFromInt(int64(qty)).
//
// Reports: the engine returns a []*Trade, so engine_on_new_order / _on_modify
// loop the returned fills and push one ME_TRADE per fill in match order. The
// canonical hash stable-sorts reports by (seq, type) (src/correctness.cpp), so
// the ack-vs-trade emission order within one message is normalised; only the
// order of multiple trades within one seq is load-bearing, and that is the
// engine's returned slice order.
//
// Cgo: package main + buildmode=c-shared so the //export functions land in the
// produced .so as plain C symbols the harness dlopen resolves directly.

package main

/*
#include <stdint.h>

// Mirror the api/matching_engine_api.h structs the cgo bridge needs. We do NOT
// #include the harness header: cgo emits engine_* prototypes without the
// const-qualified pointer args the header declares, and the C compiler then
// rejects the type mismatch. The mirrored structs must match the header
// byte-for-byte (see the static_assert block in the header).

typedef struct {
    uint64_t order_id;
    uint64_t sequence_number;
    int64_t  price_ticks;
    uint32_t quantity;
    uint8_t  side;
    uint8_t  ioc;
    uint8_t  _reserved[2];
} new_order_t;

typedef struct {
    uint64_t order_id;
    uint64_t sequence_number;
} cancel_t;

typedef struct {
    uint64_t order_id;
    uint64_t sequence_number;
    int64_t  new_price_ticks;
    uint32_t new_quantity;
    uint8_t  side;
    uint8_t  _reserved[3];
} modify_t;

// Batch element (me_msg_t): tag at offset 0, the new/cancel/modify payload at
// offset 8. The payload is mirrored as raw bytes so Go can reinterpret it per
// tag without cgo union handling. Must stay 40 bytes.
typedef struct {
    uint8_t tag;
    uint8_t _pad[7];
    uint8_t payload[32];
} me_msg_t;

enum {
    ME_ORDER_ACK     = 0,
    ME_TRADE         = 1,
    ME_CANCEL_ACK    = 2,
    ME_MODIFY_ACK    = 3,
    ME_CANCEL_REJECT = 4,
    ME_MODIFY_REJECT = 5
};

typedef struct {
    uint8_t  type;
    uint8_t  side;
    uint8_t  _reserved[6];
    uint64_t sequence_number;
    uint64_t order_id;
    int64_t  price_ticks;
    uint32_t quantity;
    uint32_t _reserved2;
    uint64_t maker_order_id;
    uint64_t taker_order_id;
    uint64_t _reserved3;
} me_report_t;

typedef struct {
    void*    (*create)(uint32_t capacity);
    int      (*push)(void* handle, const me_report_t* report);
    uint32_t (*drain)(void* handle, me_report_t* out, uint32_t max);
    void     (*flush)(void* handle);
    void     (*destroy)(void* handle);
} me_transport_t;

// Go cannot directly invoke a function-pointer field of a C struct; this shim
// performs the push for us.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}
*/
import "C"

import (
	"math"
	"strconv"
	"unsafe"

	orderbook "github.com/danielgatis/go-orderbook"
	"github.com/shopspring/decimal"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gBook *orderbook.OrderBook

	// Shadow of each resting order, keyed by the harness uint64 id. Source of
	// truth for the audit queries (best bid/ask/depth); the engine's Depth()
	// would be an O(book) scan and Bid()/Ask() are not exposed as cheap peeks.
	// Single-threaded matcher, so no mutex needed.
	gShadow map[uint64]shadowEntry
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// idStr converts a harness uint64 order id to the engine's string key. The
// engine's API is string-keyed, so this conversion is inherent to driving it
// (not an adapter shortcut). FormatUint with base 10 keeps ids dense/ordered.
func idStr(id uint64) string { return strconv.FormatUint(id, 10) }

// ---------------------------------------------------------------------------
// Report transport.
// ---------------------------------------------------------------------------

// One package-level scratch report, reused for every emission. A pointer passed
// to a cgo call escapes by default, so a per-call local would heap-allocate
// 64 B per report inside the timed window. Reuse is safe: the single matcher
// thread is the only writer, and the transport's push() copies the struct by
// value before returning. Each emitter writes its full field set (and zeroes
// the maker/taker fields the trade path uses), so the pushed bytes equal a
// fresh zero-initialised struct exactly; the _reserved fields are never written.
var gRep C.me_report_t

func emit(r *C.me_report_t) {
	for C.me_push(gTransport, gSink, r) != 1 {
		// spin until the transport accepts the report
	}
}

func emitAck(rtype C.uint8_t, seq, oid uint64,
	side uint8, price int64, qty uint32) {

	gRep._type = rtype
	gRep.sequence_number = C.uint64_t(seq)
	gRep.order_id = C.uint64_t(oid)
	gRep.side = C.uint8_t(side)
	gRep.price_ticks = C.int64_t(price)
	gRep.quantity = C.uint32_t(qty)
	gRep.maker_order_id = 0
	gRep.taker_order_id = 0
	emit(&gRep)
}

func emitTrade(seq, makerID, takerID uint64, price int64, qty uint32) {
	gRep._type = C.uint8_t(C.ME_TRADE)
	gRep.sequence_number = C.uint64_t(seq)
	gRep.order_id = C.uint64_t(makerID)
	gRep.side = 0
	gRep.price_ticks = C.int64_t(price)
	gRep.quantity = C.uint32_t(qty)
	gRep.maker_order_id = C.uint64_t(makerID)
	gRep.taker_order_id = C.uint64_t(takerID)
	emit(&gRep)
}

// emitTrades pushes one ME_TRADE per fill in match order and returns the total
// filled quantity. The maker id comes back from the engine's Trade as a string
// (it is the resting order's id, which the adapter set = idStr(uint64)), so it
// is parsed back to uint64 for the wire report. The maker price is the Trade's
// price (the resting order's price), recovered via .IntPart().
func emitTrades(seq, takerID uint64, trades []*orderbook.Trade) uint32 {
	var filled uint32
	for _, t := range trades {
		makerID, _ := strconv.ParseUint(t.MakerOrderID(), 10, 64)
		price := t.Price().IntPart()
		q := uint32(t.Amount().IntPart())
		emitTrade(seq, makerID, takerID, price, q)

		filled += q

		// Decrement the maker's shadow; retire if fully consumed.
		if e, ok := gShadow[makerID]; ok {
			if e.remaining > q {
				e.remaining -= q
			} else {
				e.remaining = 0
				e.alive = false
			}
			gShadow[makerID] = e
		}
	}
	return filled
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t,
	report_sink unsafe.Pointer) {

	gTransport = transport
	gSink = report_sink
	gShadow = make(map[uint64]shadowEntry, 1<<21)
	gBook = orderbook.NewOrderBook("HARNESS")
}

//export engine_shutdown
func engine_shutdown() {
	gBook = nil
	gShadow = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: every engine_on_* call has already matched fully
	// and pushed all its reports inline. Nothing pending.
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck (the engine accepts the new order).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	var sd orderbook.Side
	if side == 0 {
		sd = orderbook.Buy
	} else {
		sd = orderbook.Sell
	}

	// 2. Match. ProcessLimitOrder returns the fills and rests the residual.
	idS := idStr(oid)
	trades, _ := gBook.ProcessLimitOrder(idS, idS, sd,
		decimal.NewFromInt(int64(qty)), decimal.NewFromInt(price))
	filled := emitTrades(seq, oid, trades)

	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IOC: the engine rested the residual (it has no IOC mode); pull it
		// back out and emit one CancelAck for the dropped remainder.
		if residual > 0 {
			gBook.CancelOrder(idS)
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: the residual rests. Record it for cancel/modify echo + audit.
	gShadow[oid] = shadowEntry{
		price:     price,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

//export engine_on_cancel
func engine_on_cancel(c *C.cancel_t) {
	seq := uint64(c.sequence_number)
	oid := uint64(c.order_id)

	// The engine adjudicates: CancelOrder returns the removed *Order, or nil
	// if no such order is resting (never seen / already filled / already
	// cancelled). No adapter pre-check — the engine's map is the truth.
	removed := gBook.CancelOrder(idStr(oid))
	if removed == nil {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	// CancelAck. The canonical line is "2,seq,side,order_id,price_ticks"
	// (quantity omitted), so echo the removed order's side + price.
	var side uint8
	if removed.Side() == orderbook.Buy {
		side = 0
	} else {
		side = 1
	}
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid,
		side, removed.Price().IntPart(), 0)

	if e, ok := gShadow[oid]; ok {
		e.alive = false
		e.remaining = 0
		gShadow[oid] = e
	}
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	idS := idStr(oid)

	// Modify = cancel + reinsert. The cancel half adjudicates: nil removed
	// => the order is not resting => ModifyReject.
	removed := gBook.CancelOrder(idS)
	if removed == nil {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}

	side := uint8(m.side) // the order's side (modify never changes side)
	var sd orderbook.Side
	if side == 0 {
		sd = orderbook.Buy
	} else {
		sd = orderbook.Sell
	}
	if e, ok := gShadow[oid]; ok {
		e.alive = false
		e.remaining = 0
		gShadow[oid] = e
	}

	// Reinsert at the new price/quantity (loses queue priority). The reinsert
	// MAY cross resting orders on the opposite side and produce Trades.
	trades, _ := gBook.ProcessLimitOrder(idS, idS, sd,
		decimal.NewFromInt(int64(newQty)), decimal.NewFromInt(newPrice))
	filled := emitTrades(seq, oid, trades)

	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	gShadow[oid] = shadowEntry{
		price:     newPrice,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

// engine_on_batch — OPTIONAL ABI: process a run of n tagged messages in one
// C->Go crossing, amortizing the foreign-thread cgo entry cost (needm) over the
// whole run instead of paying it per message. Each message is dispatched to the
// same per-message handler, in array order, with no cross-message lookahead —
// identical semantics to one-at-a-time delivery.
//
//export engine_on_batch
func engine_on_batch(msgs *C.me_msg_t, n C.uint32_t) {
	sz := unsafe.Sizeof(C.me_msg_t{})
	base := uintptr(unsafe.Pointer(msgs))
	for i := 0; i < int(n); i++ {
		m := (*C.me_msg_t)(unsafe.Pointer(base + uintptr(i)*sz))
		payload := unsafe.Pointer(&m.payload[0])
		switch uint8(m.tag) {
		case 0:
			engine_on_new_order((*C.new_order_t)(payload))
		case 1:
			engine_on_cancel((*C.cancel_t)(payload))
		case 2:
			engine_on_modify((*C.modify_t)(payload))
		}
	}
}

// ---------------------------------------------------------------------------
// Audit queries — answered from the shadow map (the engine's Depth() is an
// O(book) walk and Bid()/Ask() peeks aren't exposed cheaply). Audit queries are
// rare, so the O(N) scan is acceptable.
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	best := int64(math.MinInt64)
	for _, e := range gShadow {
		if e.alive && e.side == 0 && e.price > best {
			best = e.price
		}
	}
	return C.int64_t(best)
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	best := int64(math.MaxInt64)
	for _, e := range gShadow {
		if e.alive && e.side == 1 && e.price < best {
			best = e.price
		}
	}
	return C.int64_t(best)
}

//export engine_query_depth_at
func engine_query_depth_at(price_ticks C.int64_t, side C.uint8_t) C.uint64_t {
	pt := int64(price_ticks)
	sd := uint8(side)
	var total uint64
	for _, e := range gShadow {
		if e.alive && e.side == sd && e.price == pt {
			total += uint64(e.remaining)
		}
	}
	return C.uint64_t(total)
}

func main() {} // required by buildmode=c-shared
