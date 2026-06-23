// wrapper.go — i25959341/orderbook behind the harness matching_engine_api.h ABI.
//
// i25959341/orderbook (github.com/i25959341/orderbook) is the canonical pure-Go
// price-time-priority limit-order book: an emirpasic/gods red-black tree of
// prices per side, each price node holding a container/list FIFO of orders, and
// an orders map[string]*list.Element for id lookup. Its matching entry point is
// ProcessLimitOrder, which RETURNS the match result (it has no per-fill
// callback). The wrapper:
//
//   - drives the engine's native ProcessLimitOrder / CancelOrder, and
//     reconstructs the harness Trade stream from ProcessLimitOrder's
//     (done, partial, partialQuantityProcessed) return values. The engine
//     reports every fully-consumed maker (in match order) in `done`, the one
//     partially-consumed maker in `partial` (+ partialQuantityProcessed for the
//     amount taken), and — when the taker fully fills — appends a synthetic
//     average-price taker order as the LAST element of `done`; the wrapper drops
//     that synthetic element and emits one Trade per real maker fill at the
//     maker's resting price. See the reconstruction proof in build.sh / README.
//   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
//     ModifyReject above the engine (the engine returns Go errors, not the
//     harness wire reports).
//   - models harness IOC as a native limit order followed by a CancelOrder of
//     the resting residual (the engine has no IOC order type), emitting the
//     IOC-residual CancelAck.
//   - models modify as cancel + reinsert (the engine has no native modify),
//     so the reinsert's crossing fills cross with the modify's sequence number.
//   - shadow-tracks {oid -> price,side,remaining,alive} ONLY for the report
//     side/price echo and the audit queries; rejects are adjudicated by the
//     engine itself (CancelOrder returns nil for a non-resting id), never by
//     the shadow.
//
// Cgo notes: built as `package main` + buildmode=c-shared so the //export
// functions land in the produced .so as plain C symbols dlopen can find.

package main

/*
#include <stdint.h>

// Mirror the ABI structs from api/matching_engine_api.h. We deliberately do NOT
// #include the harness header — cgo emits engine_* prototypes without the
// const-qualified pointer arguments the header uses, and the C compiler rejects
// the type mismatch. The structs below must match the header byte-for-byte (see
// the static_assert block in the header).

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

// Batch element (me_msg_t): tag at offset 0, payload at offset 8. Payload is
// raw bytes so Go reinterprets per tag without cgo union handling. 40 bytes.
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

// Go can't directly invoke a function-pointer field of a C struct; these tiny
// shims do the call. me_push_n hands a whole report run across in ONE Go->C
// crossing (the outbound analogue of engine_on_batch).
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
	"os"
	"strconv"
	"unsafe"

	ob "orderbook"

	"github.com/shopspring/decimal"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gBook *ob.OrderBook

	// Shadow state: side/price echo for the reports and the source of truth for
	// the audit queries. Single matcher thread, so no mutex.
	gShadow map[uint64]shadowEntry

	// Outbound bulk-emit buffer (ME_BULK_EMIT): hand a run of reports across the
	// cgo boundary in one me_push_n crossing instead of one me_push per report.
	gBulkEmit bool
	gBuf      []C.me_report_t

	// Reusable scratch report — a pointer passed to a cgo call escapes, so a
	// per-call local would heap-allocate 64 B per report inside the timed
	// window. The single matcher thread is the only writer and push() copies by
	// value, so reuse is safe; every emitter writes its full field set.
	gRep C.me_report_t
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// ---------------------------------------------------------------------------
// Conversions.
//
// Workload ticks/quantities are positive integers (the generator clamps price
// to >= 1 tick and quantity to [1,100]). decimal.New(v, 0) builds an
// integer-valued decimal whose IntPart() recovers v exactly, so the round-trip
// int64<->decimal is lossless and the tree's Cmp ordering matches integer
// ordering bit-for-bit.
// ---------------------------------------------------------------------------

func toDec(v int64) decimal.Decimal { return decimal.New(v, 0) }

func qtyFromDec(d decimal.Decimal) uint32 {
	v := d.IntPart()
	if v < 0 {
		return 0
	}
	if v > math.MaxUint32 {
		return math.MaxUint32
	}
	return uint32(v)
}

// The engine keys orders by string id; the harness ids are uint64. strconv is
// the minimal faithful bridge to the engine's native id type. idStr formats
// without allocating an intermediate []byte beyond strconv's own.
func idStr(oid uint64) string { return strconv.FormatUint(oid, 10) }

// ---------------------------------------------------------------------------
// Report emission.
// ---------------------------------------------------------------------------

func emit(r *C.me_report_t) {
	if gBulkEmit {
		gBuf = append(gBuf, *r)
		return
	}
	for C.me_push(gTransport, gSink, r) != 1 {
		// spin until the transport accepts the report
	}
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
// Fill reconstruction.
//
// emitFills converts one ProcessLimitOrder result into the harness Trade
// stream and decrements the makers' shadow. Empirically (verified against the
// engine, see README):
//
//   - Every fully-consumed maker is in `done`, in match order, each carrying
//     its own resting price and FULL consumed quantity.
//   - When the taker fully fills (quantityToTrade hit 0), the engine appends a
//     synthetic average-price order with the TAKER's id as the LAST element of
//     `done`; we must skip it.
//   - The single partially-consumed maker (if any) is in `partial`, and the
//     quantity taken from it is partialQuantityProcessed. This case occurs only
//     when the taker fully fills (a maker is left partial only if the taker ran
//     out mid-maker), so `partial` is a maker exactly when the taker filled.
//   - When the taker does NOT fully fill, `partial` is the resting taker (not a
//     maker) and every `done` element is a fully-consumed maker.
//
// Returns the total quantity the taker filled.
func emitFills(seq, takerID uint64, done []*ob.Order, partial *ob.Order,
	partialQty decimal.Decimal) uint32 {

	var filled uint32

	takerFull := len(done) > 0 && done[len(done)-1].ID() == idStr(takerID)
	n := len(done)
	if takerFull {
		n-- // drop the trailing synthetic taker order
	}

	for i := 0; i < n; i++ {
		m := done[i]
		q := qtyFromDec(m.Quantity())
		p := m.Price().IntPart()
		mid, _ := strconv.ParseUint(m.ID(), 10, 64)
		emitTrade(seq, mid, takerID, p, q)
		filled += q
		decShadow(mid, q)
	}

	if takerFull && partial != nil {
		// partial is the partially-consumed maker; partialQty came off it.
		q := qtyFromDec(partialQty)
		p := partial.Price().IntPart()
		mid, _ := strconv.ParseUint(partial.ID(), 10, 64)
		emitTrade(seq, mid, takerID, p, q)
		filled += q
		decShadow(mid, q)
	}

	return filled
}

func decShadow(makerID uint64, q uint32) {
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

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gShadow = make(map[uint64]shadowEntry, 1<<21)
	gBulkEmit = os.Getenv("ME_BULK_EMIT") != ""
	gBuf = make([]C.me_report_t, 0, 1<<20)
	gBook = ob.NewOrderBook()
}

//export engine_shutdown
func engine_shutdown() {
	gBook = nil
	gShadow = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: every report is already produced. Under ME_BULK_EMIT
	// the run sits in gBuf until handed across here.
	flushReports()
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck (the engine does not ack).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Drive the native matcher.
	var sideT ob.Side
	if side == 0 {
		sideT = ob.Buy
	} else {
		sideT = ob.Sell
	}
	done, partial, partialQty, _ := gBook.ProcessLimitOrder(sideT, idStr(oid), toDec(int64(qty)), toDec(price))

	// 3. Emit one Trade per maker fill (match order, maker price).
	filled := emitFills(seq, oid, done, partial, partialQty)

	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IOC: the engine rested any residual (it has no IOC type). Remove it
		// natively and emit the IOC-residual CancelAck.
		if residual > 0 {
			gBook.CancelOrder(idStr(oid))
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: the residual (if any) is resting in the book. Track it.
	if residual > 0 {
		gShadow[oid] = shadowEntry{price: price, side: side, remaining: residual, alive: true}
	}
}

//export engine_on_cancel
func engine_on_cancel(c *C.cancel_t) {
	seq := uint64(c.sequence_number)
	oid := uint64(c.order_id)

	// The engine adjudicates: CancelOrder returns the removed *Order, or nil if
	// no such id is resting (never seen / already filled / already cancelled).
	removed := gBook.CancelOrder(idStr(oid))
	if removed == nil {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	e := gShadow[oid]
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, e.side, removed.Price().IntPart(), qtyFromDec(removed.Quantity()))
	e.alive = false
	gShadow[oid] = e
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	// Cancel half of cancel + reinsert. Non-resting id -> ModifyReject.
	removed := gBook.CancelOrder(idStr(oid))
	if removed == nil {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := uint8(ob.Sell)
	if removed.Side() == ob.Buy {
		side = 0
	} else {
		side = 1
	}

	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	// Reinsert at the new price/qty; crossing fills cross with the modify seq.
	var sideT ob.Side
	if side == 0 {
		sideT = ob.Buy
	} else {
		sideT = ob.Sell
	}
	done, partial, partialQty, _ := gBook.ProcessLimitOrder(sideT, idStr(oid), toDec(int64(newQty)), toDec(newPrice))
	filled := emitFills(seq, oid, done, partial, partialQty)

	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	if residual > 0 {
		gShadow[oid] = shadowEntry{price: newPrice, side: side, remaining: residual, alive: true}
	} else {
		delete(gShadow, oid)
	}
}

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
// Audit queries.
//
// Read directly off the engine's own price trees / FIFO levels (read-only, no
// mutation) — the engine exposes the best-price queues and per-price volume.
// The shadow is consulted only as a fallback for depth (the engine's price-keyed
// volume is the authority).
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	if q := gBook.GetOrderSide(ob.Buy).MaxPriceQueue(); q != nil {
		return C.int64_t(q.Price().IntPart())
	}
	return C.int64_t(math.MinInt64)
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	if q := gBook.GetOrderSide(ob.Sell).MinPriceQueue(); q != nil {
		return C.int64_t(q.Price().IntPart())
	}
	return C.int64_t(math.MaxInt64)
}

//export engine_query_depth_at
func engine_query_depth_at(price_ticks C.int64_t, side C.uint8_t) C.uint64_t {
	pt := int64(price_ticks)
	var sd ob.Side
	if uint8(side) == 0 {
		sd = ob.Buy
	} else {
		sd = ob.Sell
	}
	os := gBook.GetOrderSide(sd)
	// Walk to the matching price level via the side's ordered queues. The level
	// volume is the engine's own aggregate at that price.
	want := toDec(pt)
	for q := os.MaxPriceQueue(); q != nil; q = os.LessThan(q.Price()) {
		if q.Price().Equal(want) {
			return C.uint64_t(q.Volume().IntPart())
		}
	}
	return 0
}

func main() {} // required by buildmode=c-shared
