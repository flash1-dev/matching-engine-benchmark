// wrapper.go — GOnevo/matchingo behind the harness matching_engine_api.h ABI.
//
// matchingo is a pure-Go price-time-priority limit-order book:
//   - OrderBook.orders : map[string]*Order            (id -> order)
//   - bids / asks      : *OrderSide, each a TreeSet of price levels +
//                        map[price]*OrderQueue, the queue a FIFO deque[*Order]
//   - Process(order) returns a *Done whose Trades slice is [taker, maker, maker, ...]:
//       Trades[0]  = the aggressor summary (incoming id, its own price, Processed qty)
//       Trades[1:] = one entry per resting order hit (maker id, fill qty, MAKER price)
//   - native IOC/FOK via the order's TIF; native CancelOrder(id); no in-place modify.
//
// The wrapper:
//   - converts each Trades[1:] entry into one harness Trade report (maker price,
//     aggressor seq, maker/taker ids), in match order
//   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject
//     above the engine — matchingo's Done/CancelOrder don't return a ready-made
//     report, and CancelAck/ModifyAck must echo the resting order's side+price
//   - shadow-tracks {oid -> price,side,remaining,alive} purely for that side/price
//     echo and for cancel/modify liveness adjudication (matchingo has no
//     "is this id resting?" query that distinguishes resting from filled/cancelled)
//   - answers the audit queries from the LIVE engine book (HarnessBestBid /
//     HarnessBestAsk / HarnessDepthAt, added by build.sh) — not from the shadow —
//     so engine-internal accounting (e.g. price-level volume) is exposed as-is.
//
// Modify is cancel + reinsert (matchingo has no native modify), matching the
// harness contract: the reinsert's crossing fills carry the modify's seq.
//
// Cgo notes: built as `package main` + buildmode=c-shared so the //export-tagged
// functions land in the produced .so as plain C symbols dlopen can find. We
// mirror the ABI structs in the cgo preamble rather than #include the harness
// header (cgo emits engine_* prototypes without the header's const-qualified
// pointer args, which the C compiler then rejects as a type mismatch).

package main

/*
#include <stdint.h>

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

// Go can't call a C function-pointer struct field directly; these tiny shims do.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}

// Bulk push: hand a whole report run across the cgo boundary in ONE Go->C
// crossing (the outbound analogue of engine_on_batch); per-report pushes then
// run as native C. Used by the ME_BULK_EMIT measurement path.
static inline void me_push_n(const me_transport_t* t, void* sink,
                             const me_report_t* r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        while (!t->push(sink, &r[i])) { }
}
*/
import "C"

import (
	"os"
	"strconv"
	"unsafe"

	"github.com/gonevo/matchingo"
	"github.com/nikolaydubina/fpdecimal"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gBook *matchingo.OrderBook

	// Shadow state: side+price echo for CancelAck/ModifyAck and liveness
	// adjudication for cancel/modify rejects. NOT used for the audit queries
	// (those read the live engine book). Single matcher thread => no mutex.
	gShadow map[string]shadowEntry
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// ---------------------------------------------------------------------------
// Decimal conversion.
//
// Workload prices/quantities are integers. fpdecimal.Decimal wraps a single
// int64 `v`; FromIntScaled(n) builds Decimal{v:n} directly (no *10^frac
// scaling), and Scaled() reads v back. Limit/IOC matching touches decimals only
// through Add/Sub/compare (never Mul/Div — those live on the market-quote path
// this workload never exercises), so FromIntScaled round-trips every tick and
// quantity bit-for-bit, independent of fpdecimal.FractionDigits. We map prices
// AND quantities through it so both recover exactly. Prices are always > 0 in
// the workload (NewLimitOrder panics on price <= 0), matching the harness.
// ---------------------------------------------------------------------------

func toDec(v int64) fpdecimal.Decimal { return fpdecimal.FromIntScaled(v) }

func qtyFromDec(d fpdecimal.Decimal) uint32 {
	v := d.Scaled()
	if v < 0 {
		return 0
	}
	return uint32(v)
}

// matchingo keys orders by string id. uint64 -> decimal string is the engine's
// native id form; the allocation is intrinsic to the engine's map[string]*Order
// design, not adapter-added overhead.
func idStr(id uint64) string { return strconv.FormatUint(id, 10) }

// ---------------------------------------------------------------------------
// Report transport.
// ---------------------------------------------------------------------------

// gBulkEmit (ME_BULK_EMIT) routes reports through a Go-side buffer handed across
// in one me_push_n() crossing per run instead of one me_push() per report — the
// outbound analogue of engine_on_batch. gBuf holds COPIES (gRep is reused).
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

func flushReports() {
	if len(gBuf) == 0 {
		return
	}
	C.me_push_n(gTransport, gSink, &gBuf[0], C.uint32_t(len(gBuf)))
	gBuf = gBuf[:0]
}

// One package-level scratch report, reused for every emission. A pointer passed
// to a cgo call escapes by default, so a per-call local would heap-allocate 64 B
// per report inside the timed window; `#cgo noescape` needs Go >= 1.24, newer
// than the pinned toolchain. Reuse is safe: the single matcher thread is the
// only writer and the transport's push() copies the struct by value before
// returning. Each emitter writes its full field set (zeroing the maker/taker ids
// the non-trade path leaves unused), so the pushed bytes equal the old per-call
// zero-initialised struct exactly; the _reserved fields are never written.
var gRep C.me_report_t

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
// Trade emission + shadow maintenance shared by new-order and modify.
//
// done.Trades[0] is the synthetic taker summary; Trades[1:] are the makers, each
// {maker id, fill qty, MAKER resting price}. We emit one ME_TRADE per maker, in
// match order, and decrement each maker's shadow. Returns the total filled qty so
// the caller can size the GTC resting remainder / the IOC residual.
// ---------------------------------------------------------------------------

func emitFills(done *matchingo.Done, seq, takerID uint64) uint32 {
	var filled uint32
	if done == nil { // matchingo returns (nil, ErrOrderExists) on a duplicate live id
		return 0     // — never hit by the canonical workload, but don't deref nil
	}
	trades := done.Trades
	for i := 1; i < len(trades); i++ {
		t := trades[i]
		q := qtyFromDec(t.Quantity)
		p := t.Price.Scaled()
		makerID, _ := strconv.ParseUint(t.OrderID, 10, 64)
		emitTrade(seq, makerID, takerID, p, q)
		filled += q

		if e, ok := gShadow[t.OrderID]; ok {
			if e.remaining > q {
				e.remaining -= q
			} else {
				e.remaining = 0
				e.alive = false
			}
			gShadow[t.OrderID] = e
		}
	}
	return filled
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gShadow = make(map[string]shadowEntry, 1<<21)
	gBulkEmit = os.Getenv("ME_BULK_EMIT") != ""
	gBuf = make([]C.me_report_t, 0, 1<<20)
	gBook = matchingo.NewOrderBook()
}

//export engine_shutdown
func engine_shutdown() {
	gBook = nil
	gShadow = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: engine_on_* already produced every report. Under
	// ME_BULK_EMIT they sit in gBuf until handed across here (and at the end of
	// each engine_on_batch run); otherwise they were pushed inline.
	flushReports()
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck first (engine has accepted the new order).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Drive the engine.
	var sideT matchingo.Side
	if side == 0 {
		sideT = matchingo.Buy
	} else {
		sideT = matchingo.Sell
	}
	tif := matchingo.GTC
	if o.ioc != 0 {
		tif = matchingo.IOC
	}

	done, _ := gBook.Process(
		matchingo.NewLimitOrder(idStr(oid), sideT, toDec(int64(qty)), toDec(price), tif, ""))

	filled := emitFills(done, seq, oid)
	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IOC residual: the engine already dropped it (Process cancels the
		// unfilled remainder internally). Emit the harness CancelAck for it.
		if residual > 0 {
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: the unfilled remainder rests. Shadow it for cancel/modify echo.
	gShadow[idStr(oid)] = shadowEntry{
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
	key := idStr(oid)

	e, ok := gShadow[key]
	if !ok || !e.alive {
		// Not resting — already filled, already cancelled, or never seen.
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	// Remove from the engine book; echo side/price from the shadow.
	gBook.CancelOrder(key)
	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, e.side, e.price, e.remaining)
	e.alive = false
	e.remaining = 0
	gShadow[key] = e
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newPrice := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)
	key := idStr(oid)

	e, ok := gShadow[key]
	if !ok || !e.alive {
		// Not resting — stale modify.
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := e.side // the order's resting side (== m.side in the workload)

	// Modify = cancel + reinsert. Cancel the old resting order, mark it dead so
	// the reinsert below doesn't trip matchingo's duplicate-id guard, then
	// re-add at the new price/qty. The reinsert MAY cross resting orders and
	// produce Trades (e.g. a buy repriced up through the asks) — emit those.
	gBook.CancelOrder(key)
	e.alive = false
	gShadow[key] = e

	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	var sideT matchingo.Side
	if side == 0 {
		sideT = matchingo.Buy
	} else {
		sideT = matchingo.Sell
	}
	done, _ := gBook.Process(
		matchingo.NewLimitOrder(key, sideT, toDec(int64(newQty)), toDec(newPrice), matchingo.GTC, ""))

	filled := emitFills(done, seq, oid)
	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	gShadow[key] = shadowEntry{
		price:     newPrice,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

// engine_on_batch — OPTIONAL ABI: process a run of n tagged messages in one
// C->Go crossing, amortizing the cgo foreign-thread entry cost over the run.
// Each message is dispatched to the same per-message handler, in array order,
// with no cross-message lookahead — identical semantics to one-at-a-time.
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
// Audit queries — answered from the LIVE engine book via the build.sh-added
// HarnessBestBid / HarnessBestAsk / HarnessDepthAt accessors (read-only, no
// side effects). Reading the engine directly exposes its real internal state
// (including any price-level volume accounting) to the state audit, rather than
// substituting an adapter-maintained shadow.
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	v, ok := gBook.HarnessBestBid()
	if !ok {
		return C.int64_t(math_MinInt64)
	}
	return C.int64_t(v)
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	v, ok := gBook.HarnessBestAsk()
	if !ok {
		return C.int64_t(math_MaxInt64)
	}
	return C.int64_t(v)
}

//export engine_query_depth_at
func engine_query_depth_at(price_ticks C.int64_t, side C.uint8_t) C.uint64_t {
	var sideT matchingo.Side
	if uint8(side) == 0 {
		sideT = matchingo.Buy
	} else {
		sideT = matchingo.Sell
	}
	v := gBook.HarnessDepthAt(int64(price_ticks), sideT)
	if v < 0 {
		v = 0
	}
	return C.uint64_t(v)
}

const (
	math_MinInt64 = -1 << 63
	math_MaxInt64 = 1<<63 - 1
)

func main() {} // required by buildmode=c-shared
