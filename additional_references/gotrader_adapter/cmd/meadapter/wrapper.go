// wrapper.go — robaho/go-trader's in-process limit-order book behind the
// harness matching_engine_api.h ABI.
//
// go-trader is a FIX/gRPC exchange, but its matcher (internal/exchange:
// orderBook.add / .remove / matchTrades) is a separable in-process unit: add a
// limit order, it crosses against the resting book and returns the fills; remove
// cancels by handle. This adapter drives that book directly through a thin
// exported shim (internal/exchange/me_shim.go) — no FIX session, no gRPC, no
// QuickFIX runtime. The shim is the only added engine code; it contains no
// matching logic (every cross/fill/removal is the engine's).
//
//   - go-trader returns a fill LIST from add() (not a callback); we loop it and
//     emit one ME_TRADE per fill, in match order.
//   - Trade.price_ticks is the maker's resting price — the engine already prices
//     each fill at the resting order's price (matchTrades picks the order with
//     the earlier sessionOrder.time), and the shim hands strictly-increasing
//     timestamps so the aggressor is always the newer side.
//   - IOC: submit as a limit, then the shim drops any unfilled remainder via the
//     engine's own remove(); we emit one CancelAck for it.
//   - modify = cancel + reinsert (go-trader's native ModifyOrder is exactly
//     this); the reinsert's crossing fills are emitted after the ModifyAck. A
//     modify of an already-FILLED order is REJECTED (see doModify and the
//     "Source patch" note in README.md).
//   - audit queries read the LIVE engine book (bids/asks slices), not a shadow.
//
// Built as package main + buildmode=c-shared so the //export functions land in
// the .so as plain C symbols the harness dlopen finds. engine_on_batch amortizes
// the cgo boundary crossing over a whole run (go-trader is reached through cgo).

package main

/*
#include <stdint.h>

// Mirror the api/matching_engine_api.h types the cgo bridge needs. We do NOT
// #include the harness header: cgo emits engine_* prototypes without the
// const-qualified pointer arguments the header declares, and the C compiler
// rejects the mismatch. The mirrored structs must match the header byte-for-byte
// (see the static_assert block in the header).

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

// Go cannot invoke a C function-pointer struct field directly; these shims do.
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
	"os"
	"unsafe"

	ex "github.com/robaho/go-trader/internal/exchange"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer
	gBook      *ex.MeBook
)

// ---------------------------------------------------------------------------
// Report transport. Mirrors the geseq reference: an optional Go-side buffer
// (ME_BULK_EMIT) hands a whole run across in one me_push_n crossing — the
// outbound analogue of engine_on_batch — otherwise reports push inline.
// gRep is a single reused scratch struct (a per-call local passed to a cgo call
// escapes and would heap-allocate 64 B per report inside the timed window).
// ---------------------------------------------------------------------------

var (
	gBulkEmit bool
	gBuf      []C.me_report_t
	gRep      C.me_report_t
)

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
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gBook = ex.NewMeBook()
	gBulkEmit = os.Getenv("ME_BULK_EMIT") != ""
	gBuf = make([]C.me_report_t, 0, 1<<20)
}

//export engine_shutdown
func engine_shutdown() {
	gBook = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: engine_on_* already produced every report. Under
	// ME_BULK_EMIT they sit in gBuf until handed across here (and at the end of
	// each engine_on_batch run); otherwise they were pushed inline.
	flushReports()
}

func doNewOrder(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck (the engine has accepted the new order).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Match. The shim returns fills in match order plus the resting remainder.
	fills, residual := gBook.MeAdd(int64(oid), side, price, qty, o.ioc != 0)
	for i := range fills {
		f := &fills[i]
		emitTrade(seq, uint64(f.MakerID), uint64(f.TakerID), f.PriceTicks, f.Qty)
	}

	// 3. IOC residual: one CancelAck for the unfilled remainder the shim dropped.
	if o.ioc != 0 {
		if residual > 0 {
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
	}
	// GTC residual already rests inside the engine (shim tracked the handle).
}

func doCancel(c *C.cancel_t) {
	seq := uint64(c.sequence_number)
	oid := uint64(c.order_id)

	if !gBook.MeIsLive(int64(oid)) {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}
	// Capture the resting fields for the ack payload before attempting the
	// cancel. MeCancel mirrors the engine's native CancelOrder: it rejects a
	// stale cancel of an already-filled (terminal) order — the engine's
	// CancelOrder returns OrderNotFound there — even though our live map still
	// tracks the id. Gate the ack/reject on that engine-faithful result, not on
	// live-map membership alone.
	side := gBook.MeRestingSide(int64(oid))
	price := gBook.MeRestingPrice(int64(oid))
	qty := gBook.MeRestingQty(int64(oid))
	if !gBook.MeCancel(int64(oid)) { // engine's own remove() / CancelOrder result
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, qty)
}

func doModify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	if !gBook.MeIsLive(int64(oid)) {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	// Conformance fix (go-trader #23): a modify of a fully-FILLED order must be
	// REJECTED, not swallow-acked. An id can still be in our live map after a
	// later aggressor fully consumed it (we map a fill's residual, not its
	// terminal state); upstream ModifyOrder cancels-then-reinserts and its
	// cancel of the already-removed order silently no-ops, so the modify is
	// ack'd against a phantom resting order. Gate on the order's own IsActive()
	// — symmetric with the cancel path — and reject the stale modify. See the
	// "Source patch" note in README.md.
	// https://github.com/robaho/go-trader/issues/23
	if !gBook.MeIsActive(int64(oid)) {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := gBook.MeRestingSide(int64(oid))

	// modify = cancel + reinsert. Cancel half first (engine's remove()).
	gBook.MeCancel(int64(oid))
	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	// Reinsert at the new price/qty; crossing fills are emitted after the ack.
	fills, _ := gBook.MeAdd(int64(oid), side, newPrice, newQty, false)
	for i := range fills {
		f := &fills[i]
		emitTrade(seq, uint64(f.MakerID), uint64(f.TakerID), f.PriceTicks, f.Qty)
	}
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) { doNewOrder(o) }

//export engine_on_cancel
func engine_on_cancel(c *C.cancel_t) { doCancel(c) }

//export engine_on_modify
func engine_on_modify(m *C.modify_t) { doModify(m) }

// engine_on_batch — OPTIONAL ABI: process a run of n tagged messages in one
// C->Go crossing so the cgo foreign-thread entry cost is amortized over the run
// instead of paid per order. Each message is dispatched in array order with no
// cross-message lookahead — identical semantics to one-at-a-time delivery.
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
			doNewOrder((*C.new_order_t)(payload))
		case 1:
			doCancel((*C.cancel_t)(payload))
		case 2:
			doModify((*C.modify_t)(payload))
		}
	}
	flushReports()
}

// ---------------------------------------------------------------------------
// Audit queries — read the live engine book directly.
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	if v, ok := gBook.MeBestBid(); ok {
		return C.int64_t(v)
	}
	return C.int64_t(math_MinInt64)
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	if v, ok := gBook.MeBestAsk(); ok {
		return C.int64_t(v)
	}
	return C.int64_t(math_MaxInt64)
}

//export engine_query_depth_at
func engine_query_depth_at(price_ticks C.int64_t, side C.uint8_t) C.uint64_t {
	return C.uint64_t(gBook.MeDepthAt(int64(price_ticks), uint8(side)))
}

const (
	math_MinInt64 = -1 << 63
	math_MaxInt64 = 1<<63 - 1
)

func main() {} // required by buildmode=c-shared
