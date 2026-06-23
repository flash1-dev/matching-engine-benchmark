// wrapper.go — draveness/oceanbook behind the harness matching_engine_api.h ABI.
//
// oceanbook is a pure-Go price-time matching engine: two red-black trees
// (emirpasic/gods) hold resting bids/asks keyed by (price, time, id); the
// best price is the tree's right-most node. OrderBook.InsertOrder matches an
// incoming order against the opposite book and returns []*trade.Trade (each
// carries the MAKER's resting price + maker/taker ids — exactly the harness
// Trade fields). CancelOrder removes a resting order by id.
//
// The wrapper:
//   - drives the REAL engine for all matching (InsertOrder / CancelOrder); it
//     never reimplements crossing
//   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
//     ModifyReject above the engine (oceanbook returns trades but no per-order
//     ack/reject; CancelOrder silently no-ops on a missing id, so the adapter
//     must adjudicate liveness)
//   - keeps a per-order liveness shadow {oid -> price,side,remaining,alive}
//     used ONLY to adjudicate cancel/modify rejects (oceanbook's CancelOrder
//     silently no-ops on a missing id, so the adapter must decide liveness).
//     The shadow is kept consistent with the engine by replaying the returned
//     trades: every maker fill decrements that maker's shadow remainder, and a
//     maker that reaches zero is retired — mirroring the engine retiring a
//     fully-filled maker from its own book. The audit queries do NOT read the
//     shadow: they read the engine's authoritative resting book (the Bids/Asks
//     order trees) through read-only accessors build.sh adds to the engine
//     package (see engine_query_* below), so the state audit observes the
//     engine's real resting state, not an adapter mirror of it.
//
// Modify is cancel + reinsert (oceanbook has no native modify): remove the
// resting order, then InsertOrder it at the new price/qty, crossing with the
// modify's sequence number. One ModifyAck, or ModifyReject if not resting.
//
// Time priority: oceanbook's key comparator breaks a price tie by CreatedAt
// (earlier = higher priority), then by id. The adapter stamps each inserted
// order with a strictly increasing CreatedAt so resting orders match in true
// FIFO arrival order, exactly like the liquibook baseline.
//
// Cgo notes: built as `package main` + buildmode=c-shared so the //export
// functions land in the .so as plain C symbols the harness dlopen finds. The
// engine is reached across cgo, so engine_on_batch is exported to amortize the
// C->Go crossing over a run of messages (the harness drives a foreign-runtime
// engine through it; see api/matching_engine_api.h).

package main

/*
#include <stdint.h>

// Mirror the api/matching_engine_api.h structs the cgo bridge needs. We do NOT
// #include the harness header: cgo emits engine_* prototypes without the
// const-qualified pointer args the header declares, and the C compiler then
// rejects the type mismatch. The mirrored structs must match the header
// byte-for-byte (see the static_assert block there).

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
// raw bytes so Go reinterprets it per tag without cgo union handling. 40 bytes.
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

// Go can't invoke a C function-pointer struct field directly; this shim does.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}
*/
import "C"

import (
	"math"
	"time"
	"unsafe"

	"github.com/draveness/oceanbook/pkg/order"
	"github.com/draveness/oceanbook/pkg/orderbook"
	"github.com/draveness/oceanbook/pkg/trade"
	"github.com/shopspring/decimal"
)

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract).
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gBook *orderbook.OrderBook

	// Per-order liveness shadow: side+price echo for acks, liveness for
	// cancel/modify reject adjudication, and the source of truth for audit
	// queries. Single-threaded matcher, so no mutex. VALUE-typed map (no
	// per-order pointer allocation on the resting path): the entry is a small
	// fixed struct, so storing it by value keeps the resting-order hot path
	// allocation-free (matching geseq/fmstephe/danielgatis/matchingo). A read-
	// modify-write (e := gShadow[id]; mutate e; gShadow[id] = e) updates an
	// entry in place.
	gShadow map[uint64]shadowEntry

	// Strictly increasing arrival stamp -> true FIFO time priority in the
	// engine's price-tie comparator.
	gClock int64
)

type shadowEntry struct {
	price     int64
	side      uint8 // 0 = buy (bid), 1 = sell (ask)
	remaining uint32
	alive     bool
}

// ---------------------------------------------------------------------------
// Conversions. Workload prices/quantities are positive integers; decimal.New
// (v, 0) is the exact integer decimal, so engine comparisons preserve tick
// order bit-for-bit and recovery is exact.
// ---------------------------------------------------------------------------

func toDecPrice(ticks int64) decimal.Decimal { return decimal.New(ticks, 0) }
func toDecQty(q uint32) decimal.Decimal       { return decimal.New(int64(q), 0) }

func sideToOB(side uint8) order.Side {
	if side == 0 {
		return order.SideBid
	}
	return order.SideAsk
}

// nextStamp returns a strictly increasing time used as the order CreatedAt, so
// the engine's price-tie comparator yields FIFO arrival order.
func nextStamp() time.Time {
	gClock++
	return time.Unix(0, gClock)
}

// ---------------------------------------------------------------------------
// Report transport.
// ---------------------------------------------------------------------------

// One package-level scratch report, reused for every emission. A pointer passed
// to a cgo call escapes by default (the compiler must assume C retains it), so
// a per-call local would heap-allocate 64 B per report inside the timed window;
// `#cgo noescape` needs Go >= 1.24, newer than the pinned toolchain. Reuse is
// safe: the single matcher thread is the only writer and the transport's push()
// copies the struct by value before returning. Every emitter writes its full
// field set (zeroing the fields it does not use), so the pushed bytes equal a
// fresh zero-initialised struct; the _reserved fields are never written.
var gRep C.me_report_t

func emit() {
	for C.me_push(gTransport, gSink, &gRep) != 1 {
		// spin: transport full
	}
}

func emitAck(rtype C.uint8_t, seq, oid uint64, side uint8, price int64, qty uint32) {
	gRep._type = rtype
	gRep.side = C.uint8_t(side)
	gRep.sequence_number = C.uint64_t(seq)
	gRep.order_id = C.uint64_t(oid)
	gRep.price_ticks = C.int64_t(price)
	gRep.quantity = C.uint32_t(qty)
	gRep.maker_order_id = 0
	gRep.taker_order_id = 0
	emit()
}

func emitTrade(seq, makerID, takerID uint64, price int64, qty uint32) {
	gRep._type = C.uint8_t(C.ME_TRADE)
	gRep.side = 0
	gRep.sequence_number = C.uint64_t(seq)
	gRep.order_id = C.uint64_t(makerID)
	gRep.price_ticks = C.int64_t(price)
	gRep.quantity = C.uint32_t(qty)
	gRep.maker_order_id = C.uint64_t(makerID)
	gRep.taker_order_id = C.uint64_t(takerID)
	emit()
}

// replayTrades emits one Trade per fill (in engine match order) and reconciles
// the maker shadows: each fill decrements the maker's remainder, retiring it at
// zero — mirroring the engine removing a fully-filled maker from its book. It
// returns the total quantity the taker filled.
func replayTrades(seq uint64, trades []*trade.Trade) uint32 {
	var takerFill uint32
	for _, t := range trades {
		q := uint32(t.Quantity.IntPart())
		p := t.Price.IntPart()
		emitTrade(seq, t.MakerID, t.TakerID, p, q)
		takerFill += q

		if e, ok := gShadow[t.MakerID]; ok {
			if e.remaining > q {
				e.remaining -= q
			} else {
				e.remaining = 0
				e.alive = false
			}
			gShadow[t.MakerID] = e
		}
	}
	return takerFill
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gClock = 0
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
	// Fully synchronous matcher: every report was pushed inline already.
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	price := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// 1. OrderAck (oceanbook emits no accept notification).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, price, qty)

	// 2. Drive the real engine.
	no := &order.Order{
		ID:                oid,
		Side:              sideToOB(side),
		Price:             toDecPrice(price),
		Quantity:          toDecQty(qty),
		CreatedAt:         nextStamp(),
		ImmediateOrCancel: o.ioc != 0,
	}
	trades := gBook.InsertOrder(no)
	filled := replayTrades(seq, trades)

	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IOC residual: the engine discards it (never inserted); emit the
		// CancelAck the harness expects for the unfilled remainder.
		if residual > 0 {
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, price, residual)
		}
		return
	}

	// GTC: the engine rested the remainder iff residual > 0. Shadow it.
	if residual > 0 {
		gShadow[oid] = shadowEntry{price: price, side: side, remaining: residual, alive: true}
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

	// Remove from the real engine. CancelOrder looks the order up by id in the
	// engine's own cancelOrdersQueue and removes it from the correct tree.
	gBook.CancelOrder(&order.Order{ID: oid})

	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, e.side, e.price, e.remaining)
	e.alive = false
	delete(gShadow, oid)
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

	// Cancel half: remove the resting order from the engine.
	gBook.CancelOrder(&order.Order{ID: oid})
	e.alive = false
	delete(gShadow, oid)

	// ModifyAck, then reinsert at the new price/qty (loses time priority).
	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newPrice, newQty)

	no := &order.Order{
		ID:        oid,
		Side:      sideToOB(side),
		Price:     toDecPrice(newPrice),
		Quantity:  toDecQty(newQty),
		CreatedAt: nextStamp(),
	}
	trades := gBook.InsertOrder(no)
	filled := replayTrades(seq, trades)

	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	if residual > 0 {
		gShadow[oid] = shadowEntry{price: newPrice, side: side, remaining: residual, alive: true}
	}
}

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
// Audit queries — read the LIVE engine book, not the shadow.
//
// oceanbook exposes no public numeric best-bid/ask/depth accessor: the best
// price lives in the unexported Bids/Asks red-black trees, and its only depth
// surface (Depth.Serialize) returns protobuf strings. build.sh adds one
// read-only file to the engine package (harness_query.go, same package
// orderbook) exporting three side-effect-free accessors that read exactly what
// the engine maintains — best = the price tree's right-most node (the same node
// the match loop crosses against via makerBooks.Right()), depth = the engine's
// own per-price-level depth accounting. So the state audit observes the
// engine's REAL internal depth — the field the upstream #44 Depth fix corrects
// — not an adapter shadow that would paper over the bug. The shadow remains the
// source of truth only for cancel/modify reject adjudication (oceanbook's
// CancelOrder silently no-ops on a missing id and returns nothing).
// ---------------------------------------------------------------------------

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t {
	if gBook == nil {
		return C.int64_t(int64(math.MinInt64))
	}
	if p, ok := gBook.HarnessBestBid(); ok {
		return C.int64_t(p)
	}
	return C.int64_t(int64(math.MinInt64))
}

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t {
	if gBook == nil {
		return C.int64_t(int64(math.MaxInt64))
	}
	if p, ok := gBook.HarnessBestAsk(); ok {
		return C.int64_t(p)
	}
	return C.int64_t(int64(math.MaxInt64))
}

//export engine_query_depth_at
func engine_query_depth_at(price_ticks C.int64_t, side C.uint8_t) C.uint64_t {
	if gBook == nil {
		return 0
	}
	return C.uint64_t(gBook.HarnessDepthAt(int64(price_ticks), sideToOB(uint8(side))))
}

func main() {} // required by buildmode=c-shared
