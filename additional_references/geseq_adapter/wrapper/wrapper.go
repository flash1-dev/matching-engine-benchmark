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
//   - shadow-tracks {oid -> price,side,remaining,alive} for the side/price
//     echo, and as the source of truth for the audit queries (Bid/Ask on the
//     engine itself would consume the token budget and can't be called as a
//     "read-only" peek); rejects are adjudicated by the engine's PutOrder
//     cancel verdict, not the shadow
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

// Batch element (api/matching_engine_api.h me_msg_t): tag at 0, the new/cancel/
// modify payload at offset 8. The payload is mirrored as raw bytes so Go can
// reinterpret it per tag without cgo union handling. Must stay 40 bytes.
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

// Tiny shim: Go can call a C function but can't directly invoke a function
// pointer field of a C struct; this wrapper does the call for us.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}

// Bulk push: hand a whole report run across the cgo boundary in ONE Go->C
// crossing; the per-report transport pushes then run as native C. Used by the
// ME_BULK_EMIT measurement path to amortize the OUTBOUND crossing the way
// engine_on_batch amortizes the inbound one.
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

	// Shadow state. The side+price echo and the audit queries read from
	// here. Single-threaded matcher, so no mutex needed.
	gShadow map[uint64]shadowEntry

	// Per-call context: PutTrade reads gCurSeq and accumulates into gTakerFill.
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
// Workload ticks are positive signed integers (the canonical scenarios span
// [10494, 42817]). We carry each tick
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

// gBulkEmit (ME_BULK_EMIT) routes reports through a Go-side buffer that is
// handed across in one me_push_n() crossing per run, instead of one me_push()
// crossing per report — the outbound analogue of engine_on_batch. gBuf holds
// COPIES (gRep is reused, so the value is snapshotted on append).
var (
	gBulkEmit bool
	gBuf      []C.me_report_t
)

func emit(r *C.me_report_t) {
	if gBulkEmit {
		gBuf = append(gBuf, *r)
		return
	}
	for {
		if C.me_push(gTransport, gSink, r) == 1 {
			return
		}
		// spin until the transport accepts the report
	}
}

// flushReports hands the buffered run across the boundary in one crossing.
func flushReports() {
	if len(gBuf) == 0 {
		return
	}
	C.me_push_n(gTransport, gSink, &gBuf[0], C.uint32_t(len(gBuf)))
	gBuf = gBuf[:0]
}

// One package-level scratch report, reused for every emission. A pointer
// passed to a cgo call escapes by default (the compiler must assume C keeps
// it), so a per-call local would heap-allocate 64 B per report inside the
// timed window; `#cgo noescape` needs Go >= 1.24, newer than the pinned
// toolchain. Reuse is safe: the single matcher thread is the only writer and
// the transport's push() copies the struct by value before returning. Each
// emitter writes its full field set and zeroes the fields only the other
// writes, so the pushed bytes equal the old per-call zero-initialised struct
// exactly (the _reserved fields are never written and stay zero).
var gRep C.me_report_t

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

// ---------------------------------------------------------------------------
// NotificationHandler.
//
// The engine fires:
//  - PutOrder(MsgCreateOrder, Accepted, ...) once per AddOrder, before
//    matching. We ignore this — engine_on_new_order eagerly emits OrderAck
//    with the side+price the callback doesn't carry.
//  - PutTrade(maker, taker, ..., qty, price) per fill.
//  - PutOrder(MsgCancelOrder, ...) once per public CancelOrder (not for
//    fully-filled makers internally retired during matching): the engine's
//    cancel verdict. Recorded into gCancelOK/gCancelQty below — it
//    adjudicates every cancel and every modify's cancel half; engine_on_cancel
//    emits the CancelAck itself with the side/price the callback lacks.
//  - PutOrder(MsgCreateOrder, Rejected, ...) on duplicate-ID / zero-qty /
//    zero-price / no-matching errors. The canonical workload doesn't
//    produce these, but we ignore them defensively.
// ---------------------------------------------------------------------------

type harnessHandler struct{}

// The engine's native cancel verdict, recorded here and consumed immediately
// after each gBook.CancelOrder (the handler fires synchronously inside it on
// the matcher thread): Canceled = removed (qty = the engine-reported
// remaining quantity), Rejected(ErrOrderNotExists) = never seen or already
// terminal. This callback IS the engine's cancel-result API — the adapter
// adjudicates every cancel (and every modify's cancel half) from it.
var (
	gCancelOK  bool
	gCancelQty uint32
)

func (h *harnessHandler) PutOrder(m ob.MsgType, s ob.OrderStatus,
	orderID uint64, qty udecimal.Decimal, err error) {
	if m == ob.MsgCancelOrder {
		gCancelOK = s == ob.Canceled
		gCancelQty = fromDecQty(qty)
	}
	// Create/accept notifications carry no payload the adapter needs; the
	// OrderAck is synthesised above the engine.
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
		gShadow[makerID] = e
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
	gShadow = make(map[uint64]shadowEntry, 1<<21)
	gBulkEmit = os.Getenv("ME_BULK_EMIT") != ""
	gBuf = make([]C.me_report_t, 0, 1<<20)

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
	// Synchronous matcher: engine_on_* has already produced every report.
	// Under ME_BULK_EMIT they sit in gBuf until handed across here (and at the
	// end of each engine_on_batch run); otherwise they were pushed inline.
	flushReports()
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
	flag := ob.None
	if o.ioc != 0 {
		flag = ob.IoC
	}

	gTok++
	gBook.AddOrder(gTok, oid, ob.Limit, sideT,
		toDecQty(qty), toDecPrice(price), udecimal.Zero, flag)

	filled := gTakerFill
	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IoC residual cancellation: emit a CancelAck for the unfilled
		// remainder. The engine discards the residual without notifying.
		if residual > 0 {
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

	// The engine adjudicates: CancelOrder answers through the notification
	// handler — Canceled on removal, Rejected(ErrOrderNotExists) for
	// never-seen and already-terminal ids alike. No adapter pre-check.
	gTok++
	gCancelOK = false
	gBook.CancelOrder(gTok, oid)
	if !gCancelOK {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	// Payload echo: side/price from the shadow (the engine's callback
	// carries neither); the quantity is the engine's own reported remainder.
	e := gShadow[oid]
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid,
		e.side, e.price, gCancelQty)
	e.alive = false // audit queries scan alive/remaining
	gShadow[oid] = e
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	// The engine adjudicates the cancel half of cancel + reinsert: Rejected
	// (never seen / already terminal) maps to ModifyReject. No pre-check.
	gTok++
	gCancelOK = false
	gBook.CancelOrder(gTok, oid)
	if !gCancelOK {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := gShadow[oid].side // payload echo (engine callback has no side)

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
	gShadow[oid] = shadowEntry{
		price:     newPrice,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

// engine_on_batch — OPTIONAL ABI: process a run of n tagged messages in one
// C->Go crossing, so the foreign-thread cgo entry cost (needm /proc/self/maps)
// is amortized over the whole run instead of paid per order. Each message is
// dispatched to the same per-message handler, in array order, with no
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
			engine_on_new_order((*C.new_order_t)(payload))
		case 1:
			engine_on_cancel((*C.cancel_t)(payload))
		case 2:
			engine_on_modify((*C.modify_t)(payload))
		}
	}
	// Outbound amortization (ME_BULK_EMIT): hand this run's reports across in one
	// crossing. No-op when bulk emit is off (reports were pushed inline).
	flushReports()
}

// ---------------------------------------------------------------------------
// Audit queries.
//
// best_bid/best_ask forward to the engine's own Bid()/Ask(): each is
// token-consuming (it CASes lastToken just like AddOrder/CancelOrder), so the
// adapter advances the same gTok counter it already owns for every engine
// call and passes the new token in — the identical discipline used for every
// mutating call, not a separate "read-only" mode. This answers the
// book-state audit from the engine's own resting orders instead of the
// notification-fed shadow. A nil return (no resting order on that side) maps
// to the MinInt64/MaxInt64 empty-book sentinel.
//
// depth_at stays on the shadow map below: the engine exposes no per-price
// accessor (Bid()/Ask() only ever return the single best order on a side),
// so there is no engine state to forward an arbitrary-price query to.
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	gTok++
	o := gBook.Bid(gTok)
	if o == nil {
		return C.int64_t(math.MinInt64)
	}
	return C.int64_t(fromDecPrice(o.Price))
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	gTok++
	o := gBook.Ask(gTok)
	if o == nil {
		return C.int64_t(math.MaxInt64)
	}
	return C.int64_t(fromDecPrice(o.Price))
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
