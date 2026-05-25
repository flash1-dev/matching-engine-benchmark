// wrapper.go — geseq/orderbook behind the harness matching_engine_api.h ABI.
//
// geseq/orderbook is a pure-Go limit-order book with a callback-style
// NotificationHandler interface (PutOrder for accept/reject/cancel,
// PutTrade for each fill). The wrapper:
//   - implements NotificationHandler to convert PutTrade into harness Trade
//     reports
//   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
//     ModifyReject above the engine — the engine's PutOrder callbacks lack
//     side/price, which the harness wire format requires
//   - drives the engine's mandatory monotonic token via a per-call counter
//   - shadow-tracks {oid -> price,side,remaining,alive} for the reject path
//     and the side/price echo, and as the source of truth for the audit
//     queries (Bid/Ask on the engine itself would consume the token budget
//     and can't be called as a "read-only" peek)
//
// Modify is cancel + reinsert (the engine has no native modify; this matches
// the harness contract: fills cross with the modify's seq).
//
// Cgo notes: this file is built as `package main` + buildmode=c-shared so the
// //export-tagged functions land in the produced .so as plain C symbols that
// the harness's dlopen can find.

package main

/*
#include <stdint.h>

// Mirror the types from api/matching_engine_api.h that the cgo bridge needs.
// We deliberately do NOT #include the harness header — cgo emits engine_*
// prototypes without the const-qualified pointer arguments the header uses,
// and the C compiler rejects the resulting type mismatch. Mirroring the
// structs (which must match the header byte-for-byte; see the static_assert
// block in the header for the layout requirements) sidesteps that.

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

// Tiny shim: Go can call a C function but can't directly invoke a function
// pointer field of a C struct; this wrapper does the call for us.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}
*/
import "C"

import (
	"math"
	"unsafe"

	ob "github.com/geseq/orderbook"
	"github.com/geseq/udecimal"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gBook *ob.OrderBook
	gTok  uint64 // monotonic token for AddOrder/CancelOrder; engine CAS's it

	// Shadow state. The reject path / side+price echo / audit queries all
	// read from here. Single-threaded matcher, so no mutex needed.
	gShadow map[uint64]*shadowEntry

	// Per-call context PutTrade reads.
	gCurSeq    uint64
	gTakerFill uint32
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// ---------------------------------------------------------------------------
// Decimal conversions.
//
// Workload ticks are signed integers in [26920, 64843]. We carry each tick
// as udecimal.New(uint64(tick), 0) which produces internal fp = tick * 10^8.
// The engine compares fp directly, so a strictly increasing tick mapping is
// preserved bit-for-bit; d.Int() recovers fp / 10^8 = tick exactly.
// ---------------------------------------------------------------------------

func toDecPrice(ticks int64) udecimal.Decimal {
	if ticks <= 0 {
		return udecimal.Zero
	}
	return udecimal.New(uint64(ticks), 0)
}

func toDecQty(q uint32) udecimal.Decimal {
	return udecimal.New(uint64(q), 0)
}

func fromDecPrice(d udecimal.Decimal) int64 {
	return int64(d.Int())
}

func fromDecQty(d udecimal.Decimal) uint32 {
	v := d.Int()
	if v > math.MaxUint32 {
		return math.MaxUint32
	}
	return uint32(v)
}

// ---------------------------------------------------------------------------
// Report transport.
// ---------------------------------------------------------------------------

func emit(r *C.me_report_t) {
	for {
		if C.me_push(gTransport, gSink, r) == 1 {
			return
		}
		// spin until the transport accepts the report
	}
}

func emitAck(rtype C.uint8_t, seq, oid uint64,
	side uint8, price int64, qty uint32) {

	var r C.me_report_t
	r._type = rtype
	r.sequence_number = C.uint64_t(seq)
	r.order_id = C.uint64_t(oid)
	r.side = C.uint8_t(side)
	r.price_ticks = C.int64_t(price)
	r.quantity = C.uint32_t(qty)
	emit(&r)
}

func emitTrade(seq, makerID, takerID uint64, price int64, qty uint32) {
	var r C.me_report_t
	r._type = C.uint8_t(C.ME_TRADE)
	r.sequence_number = C.uint64_t(seq)
	r.order_id = C.uint64_t(makerID)
	r.price_ticks = C.int64_t(price)
	r.quantity = C.uint32_t(qty)
	r.maker_order_id = C.uint64_t(makerID)
	r.taker_order_id = C.uint64_t(takerID)
	emit(&r)
}

// ---------------------------------------------------------------------------
// NotificationHandler.
//
// The engine fires:
//  - PutOrder(MsgCreateOrder, Accepted, ...) once per AddOrder, before
//    matching. We ignore this — engine_on_new_order eagerly emits OrderAck
//    with the side+price the callback doesn't carry.
//  - PutTrade(maker, taker, ..., qty, price) per fill.
//  - PutOrder(MsgCancelOrder, Canceled, ...) once per public CancelOrder
//    (not for fully-filled makers internally retired during matching).
//    The harness needs side/price echoed; engine_on_cancel emits CancelAck
//    itself, so we ignore this callback too.
//  - PutOrder(MsgCreateOrder, Rejected, ...) on duplicate-ID / zero-qty /
//    zero-price / no-matching errors. The canonical workload doesn't
//    produce these, but we ignore them defensively.
// ---------------------------------------------------------------------------

type harnessHandler struct{}

func (h *harnessHandler) PutOrder(m ob.MsgType, s ob.OrderStatus,
	orderID uint64, qty udecimal.Decimal, err error) {
	// Synthesised above the engine. Nothing to do here.
}

func (h *harnessHandler) PutTrade(makerID, takerID uint64,
	makerStatus, takerStatus ob.OrderStatus,
	qty, price udecimal.Decimal) {

	q := fromDecQty(qty)
	p := fromDecPrice(price)

	emitTrade(gCurSeq, makerID, takerID, p, q)
	gTakerFill += q

	// Decrement the maker's shadow; retire if fully filled.
	if e, ok := gShadow[makerID]; ok {
		if e.remaining >= q {
			e.remaining -= q
		} else {
			e.remaining = 0
		}
		if e.remaining == 0 {
			e.alive = false
		}
	}
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t,
	report_sink unsafe.Pointer) {

	gTransport = transport
	gSink = report_sink
	gTok = 0
	gShadow = make(map[uint64]*shadowEntry, 1<<21)

	h := &harnessHandler{}
	gBook = ob.NewOrderBook(h,
		ob.WithMatching(true),
		ob.WithOrderPoolSize(1<<21),
		ob.WithNodeTreePoolSize(1<<21),
		ob.WithOrderTreeNodePoolSIze(1<<21),
		ob.WithOrderQueuePoolSize(1<<16),
	)
}

//export engine_shutdown
func engine_shutdown() {
	gBook = nil
	gShadow = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: engine_on_* has already pushed every report
	// before returning.
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck. (Engine will fire Accepted too; we ignore that.)
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Drive the engine. PutTrade reads gCurSeq and writes gTakerFill.
	gCurSeq = seq
	gTakerFill = 0

	var sideT ob.SideType
	if side == 0 {
		sideT = ob.Buy
	} else {
		sideT = ob.Sell
	}
	flag := ob.FlagType(ob.None)
	if o.ioc != 0 {
		flag = ob.IoC
	}

	gTok++
	gBook.AddOrder(gTok, oid, ob.Limit, sideT,
		toDecQty(qty), toDecPrice(price), udecimal.Zero, flag)

	filled := gTakerFill

	if o.ioc != 0 {
		// IoC residual cancellation: emit a CancelAck for the unfilled
		// remainder. The engine discards the residual without notifying.
		var residual uint32
		if filled < qty {
			residual = qty - filled
		}
		if residual > 0 {
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: shadow tracks the resting remainder.
	var residual uint32
	if filled < qty {
		residual = qty - filled
	}
	gShadow[oid] = &shadowEntry{
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

	e, ok := gShadow[oid]
	if !ok || !e.alive {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	gTok++
	gBook.CancelOrder(gTok, oid)

	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid,
		e.side, e.price, e.remaining)
	e.alive = false
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	e, ok := gShadow[oid]
	if !ok || !e.alive {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := e.side

	// Cancel the resting order, then reinsert at the new price/qty so any
	// crossing fills carry the modify's seq.
	gTok++
	gBook.CancelOrder(gTok, oid)
	e.alive = false

	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	gCurSeq = seq
	gTakerFill = 0

	var sideT ob.SideType
	if side == 0 {
		sideT = ob.Buy
	} else {
		sideT = ob.Sell
	}
	gTok++
	gBook.AddOrder(gTok, oid, ob.Limit, sideT,
		toDecQty(newQty), toDecPrice(newPrice), udecimal.Zero, ob.None)

	filled := gTakerFill
	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	gShadow[oid] = &shadowEntry{
		price:     newPrice,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

// ---------------------------------------------------------------------------
// Audit queries.
//
// The engine's Bid()/Ask() are token-consuming (they CAS lastToken just like
// AddOrder), so we cannot use them as read-only peeks without burning a
// token slot and corrupting subsequent dispatch. The shadow map is the
// source of truth: O(N) scan but audit queries are rare.
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	best := int64(math.MinInt64)
	for _, e := range gShadow {
		if e.alive && e.side == 0 && e.price > best {
			best = e.price
		}
	}
	if best == math.MinInt64 {
		return C.int64_t(math.MinInt64)
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
	if best == math.MaxInt64 {
		return C.int64_t(math.MaxInt64)
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
