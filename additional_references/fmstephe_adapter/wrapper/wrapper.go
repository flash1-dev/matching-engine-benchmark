// wrapper.go — fmstephe/matching_engine behind the harness
// matching_engine_api.h ABI.
//
// fmstephe/matching_engine is a pure-Go price-time-priority limit-order book.
// Buy and sell orders live in two red-black trees keyed by price (with a FIFO
// queue of orders at each price), plus a third "orders" tree keyed by a 64-bit
// guid for cancel-by-id. The matcher is driven in-process, synchronously, on
// the calling thread:
//
//     m := matcher.NewMatcher(slabSize)
//     m.Config("adapter", in, out)   // in unused; out captures emitted reports
//     m.Submit(&msg)                 // BUY / SELL / CANCEL
//
// m.Submit reads msg.Kind and runs the match on the calling thread; every
// report the engine produces is written to the configured `out` MsgWriter.
// This wrapper supplies a capturing MsgWriter and translates the engine's
// output messages into the harness six-report stream.
//
// Order identity. The engine has no separate order-id; it identifies an order
// by guid = CombineInt32(TraderId, TradeId) = (TraderId<<32)|TradeId. The
// harness order_id fits in 32 bits, so we map order_id -> TraderId, TradeId=1,
// giving a unique guid per order_id. Cancels reconstruct the same guid from the
// same TraderId/TradeId.
//
// Trade price. IMPORTANT: the engine prints trades at the MIDPOINT of the
// crossing bid and ask (matcher.go price(): sPrice+(bPrice-sPrice)/2), not at
// the maker's resting price the harness wire format requires. The midpoint only
// sets the printed price; it does NOT affect which orders match or the resting
// book state (the match predicate is b.Price() >= s.Price()). This wrapper
// reports the MAKER'S resting price (known from the shadow) — the harness
// trade-price convention, the same correction the bundled baselines apply for
// trade-price-convention mismatches. The midpoint behaviour is reported as a
// convention deviation in the audit, NOT hidden: it is a real divergence from
// price-time priority, where a marketable order should print at the resting
// (maker) price.
//
// IOC. The engine has no IOC order type. We submit IOC as a normal limit, let
// it match, then explicitly cancel any resting remainder and emit a CancelAck
// for it (the harness IOC-residual contract). Modify is cancel + reinsert.
//
// Cgo notes: built as `package main` + buildmode=c-shared so the //export
// functions land in the .so as plain C symbols dlopen can find.

package main

/*
#include <stdint.h>

// Mirror the api/matching_engine_api.h structs (byte-for-byte; see the header's
// static_assert block). We do NOT #include the harness header: cgo emits
// engine_* prototypes without the const-qualified pointer args the header uses,
// and the C compiler rejects the type mismatch. Mirroring sidesteps that.

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

// Batch element (me_msg_t): tag at 0, payload at offset 8. 40 bytes total.
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

// Go can't invoke a C function-pointer struct field directly; these shims do.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}
static inline void me_push_n(const me_transport_t* t, void* sink,
                             const me_report_t* r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        while (!t->push(sink, &r[i])) { }
}
*/
import "C"

import (
	"math"
	"unsafe"

	"github.com/fmstephe/matching_engine/matcher"
	"github.com/fmstephe/matching_engine/msg"
)

const stockId = uint64(1) // single book, single symbol (harness contract)

// ---------------------------------------------------------------------------
// Capturing MsgWriter: the matcher writes every report here on the calling
// thread; we drain it after each Submit. Single-threaded, no locks.
// ---------------------------------------------------------------------------

type captureWriter struct {
	msgs []msg.Message
}

func (w *captureWriter) Read() msg.Message { return msg.Message{} }
func (w *captureWriter) Write(m msg.Message) {
	w.msgs = append(w.msgs, m)
}

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gMatcher *matcher.M
	gCapture *captureWriter

	// Shadow of each resting order: source of truth for cancel/modify
	// side+price echo, the audit queries, AND the maker's trade price.
	gShadow map[uint64]shadowEntry

	// Outbound bulk-emit buffer (amortizes the cgo crossing per batch run).
	gBuf []C.me_report_t
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// ---------------------------------------------------------------------------
// order_id <-> engine (TraderId, TradeId) guid mapping.
//   guid = (order_id << 32) | 1
// order_id fits in 32 bits, TradeId is the constant 1 -> guid unique per id.
// ---------------------------------------------------------------------------

func traderIdOf(oid uint64) uint32 { return uint32(oid) }

const tradeId = uint32(1)

// ---------------------------------------------------------------------------
// Report transport.
// ---------------------------------------------------------------------------

var gRep C.me_report_t

func emit(r *C.me_report_t) {
	gBuf = append(gBuf, *r)
}

func flushReports() {
	if len(gBuf) == 0 {
		return
	}
	C.me_push_n(gTransport, gSink, &gBuf[0], C.uint32_t(len(gBuf)))
	gBuf = gBuf[:0]
}

func emitAck(rtype C.uint8_t, seq, oid uint64, side uint8, price int64, qty uint32) {
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

// ---------------------------------------------------------------------------
// Trade processing.
//
// The matcher writes two messages per fill via completeTrade(brk, srk, b, s):
// a buy-side message {b.TraderId,...} then a sell-side message {s.TraderId,...},
// both carrying the engine's MIDPOINT price and the fill amount. We pair them
// (buy, sell) and emit ONE harness Trade. The maker is the resting order — the
// one on the side OPPOSITE the aggressor — and we report its shadow price.
// Returns the total quantity the aggressor filled.
// ---------------------------------------------------------------------------

func processTrades(seq, aggressorOID uint64, aggressorSide uint8) uint32 {
	var filled uint32
	out := gCapture.msgs
	for i := 0; i+1 < len(out); i += 2 {
		buyMsg := out[i]    // completeTrade always writes the buy side first
		sellMsg := out[i+1] // then the sell side
		amount := uint32(buyMsg.Amount)

		buyOID := uint64(buyMsg.TraderId)
		sellOID := uint64(sellMsg.TraderId)

		var makerOID, takerOID uint64
		if aggressorSide == 0 { // aggressor bought -> sell side is the maker
			makerOID, takerOID = sellOID, buyOID
		} else { // aggressor sold -> buy side is the maker
			makerOID, takerOID = buyOID, sellOID
		}

		makerPrice := gShadow[makerOID].price

		emitTrade(seq, makerOID, takerOID, makerPrice, amount)
		filled += amount

		// Decrement the maker's shadow; retire if fully filled.
		if e, ok := gShadow[makerOID]; ok {
			if e.remaining > amount {
				e.remaining -= amount
			} else {
				e.remaining = 0
				e.alive = false
			}
			gShadow[makerOID] = e
		}
	}
	gCapture.msgs = gCapture.msgs[:0]
	return filled
}

// submitOrder drives one BUY/SELL through the matcher and returns the filled
// quantity. The aggressor's resting remainder (qty - filled) is handled by the
// caller (rest in shadow for GTC, cancel for IOC).
func submitOrder(seq, oid uint64, side uint8, price int64, qty uint32) uint32 {
	gCapture.msgs = gCapture.msgs[:0]
	m := msg.Message{
		Price:    uint64(price),
		Amount:   uint64(qty),
		StockId:  stockId,
		TraderId: traderIdOf(oid),
		TradeId:  tradeId,
	}
	if side == 0 {
		m.Kind = msg.BUY
	} else {
		m.Kind = msg.SELL
	}
	gMatcher.Submit(&m)
	return processTrades(seq, oid, side)
}

// submitCancel drives a CANCEL through the matcher. Returns true if the engine
// removed a resting order (CANCELLED), false if it was not resting
// (NOT_CANCELLED).
func submitCancel(oid uint64) bool {
	gCapture.msgs = gCapture.msgs[:0]
	m := msg.Message{
		Kind:     msg.CANCEL,
		Amount:   1,
		StockId:  stockId,
		TraderId: traderIdOf(oid),
		TradeId:  tradeId,
	}
	gMatcher.Submit(&m)
	ok := false
	for _, r := range gCapture.msgs {
		if r.Kind == msg.CANCELLED {
			ok = true
		}
	}
	gCapture.msgs = gCapture.msgs[:0]
	return ok
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gShadow = make(map[uint64]shadowEntry, 1<<21)
	gBuf = make([]C.me_report_t, 0, 1<<20)

	gCapture = &captureWriter{msgs: make([]msg.Message, 0, 64)}
	// Slab sizing: a generous static pool of resting OrderNodes (the canonical
	// workload rests far fewer than this at once; overflow falls back to the GC
	// heap inside the engine's Slab.Malloc). This is a one-time capacity
	// reserve, the static-allocation parity a flat-array engine gets at init.
	gMatcher = matcher.NewMatcher(1 << 21)
	gMatcher.Config("fmstephe-adapter", gCapture, gCapture)
}

//export engine_shutdown
func engine_shutdown() {
	gMatcher = nil
	gShadow = nil
	gCapture = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: every report is already produced. Under inline emit
	// they sit in gBuf until handed across here (and at the end of each batch).
	flushReports()
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck (engine emits no accept message; synthesise it).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Match.
	filled := submitOrder(seq, oid, side, price, qty)

	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IOC residual: the engine rested it (no native IOC); cancel it and
		// emit a CancelAck for the dropped remainder.
		if residual > 0 {
			submitCancel(oid)
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: shadow tracks the resting remainder.
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

	if !submitCancel(oid) {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}
	e := gShadow[oid]
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, e.side, e.price, e.remaining)
	e.alive = false
	e.remaining = 0
	gShadow[oid] = e
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	// Cancel half of cancel + reinsert. Not resting -> ModifyReject.
	if !submitCancel(oid) {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := gShadow[oid].side // engine output carries side; shadow is simplest

	// ModifyAck for the accepted modify.
	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	// Reinsert at the new price/qty; crossing fills emit Trades with this seq.
	filled := submitOrder(seq, oid, side, newPrice, newQty)
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
// C->Go crossing, amortizing the cgo entry cost over the run. Each message is
// dispatched in array order with no cross-message lookahead — identical
// semantics to one-at-a-time delivery.
//
//export engine_on_batch
func engine_on_batch(msgs *C.me_msg_t, n C.uint32_t) {
	sz := unsafe.Sizeof(C.me_msg_t{})
	base := uintptr(unsafe.Pointer(msgs))
	for i := 0; i < int(n); i++ {
		mm := (*C.me_msg_t)(unsafe.Pointer(base + uintptr(i)*sz))
		payload := unsafe.Pointer(&mm.payload[0])
		switch uint8(mm.tag) {
		case 0:
			engine_on_new_order((*C.new_order_t)(payload))
		case 1:
			engine_on_cancel((*C.cancel_t)(payload))
		case 2:
			engine_on_modify((*C.modify_t)(payload))
		}
	}
	flushReports()
}

// ---------------------------------------------------------------------------
// Audit queries — answered from the shadow (the engine exposes no read-only
// best-bid/ask/depth API). O(N) scan; audit queries are rare.
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
