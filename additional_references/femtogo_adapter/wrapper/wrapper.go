// wrapper.go — ejyy/femto_go behind the harness matching_engine_api.h ABI.
//
// femto_go (https://github.com/ejyy/femto_go) is a pure-Go, multi-symbol,
// price-time-priority limit order book built around two SPSC ring buffers
// (an input command ring and an output event ring). Its public surface is
// MatchingEngine.Limit(...) and MatchingEngine.Cancel(...); there is no
// native modify. Results are reported as OutputEvents on outputRing, which
// the demo in main.go drains on a separate goroutine. This adapter does NOT
// use that goroutine: it calls Limit/Cancel synchronously on the matcher
// thread and drains outputRing inline (RingBuffer.Read spins on an empty
// ring, so we read writePos/readPos directly instead), turning
// EXECUTION_EVENTs into harness Trade reports and consuming the rest for
// their side channels (ORDER_EVENT -> id mapping, CANCEL/REJECT_EVENT ->
// cancel verdicts).
//
// Why the engine source is copied in (not imported): femto_go is `package
// main`, which cannot be imported at all, and the fields the wrapper must
// reach (`outputRing`, and the ring's `writePos`/`readPos`/`buffer`) are
// unexported. build.sh copies the pinned-SHA engine .go files (everything
// except main.go and *_test.go) into this directory so they compile together
// as one `package main`, giving the wrapper direct access to MatchingEngine,
// Limit/Cancel and outputRing. No engine source is modified.
//
// Two impedance mismatches the adapter bridges:
//
//   1. ENGINE-ASSIGNED ORDER IDS. femto_go ignores any caller id and assigns
//      its own monotonic OrderID (e.orderID++) per accepted Limit; cancels
//      take that engine id. The harness supplies its own order_id. The
//      adapter maps harness_oid <-> engine_oid in both directions — engine ->
//      harness to translate a trade's maker id back to a harness id (the
//      taker's harness id is the in-flight call's own parameter),
//      and harness -> engine to drive Cancel and the modify's cancel step.
//      The engine id of a new order is read out of the ORDER_EVENT the engine
//      emits first (rejected orders do NOT consume an engine id, so reading
//      the event is exact).
//
//   2. PRICE RANGE. femto_go prices are a uint32 array index in [1,
//      MAX_PRICE_LEVELS-1] = [1, 16383]; the harness workload ticks are ~32k.
//      The adapter shifts every tick by a fixed offset so the workload band
//      lands in the middle of the engine's index space, order-preservingly
//      (a strictly increasing, injective map — matching semantics are
//      unchanged). Ticks that fall outside the representable window are
//      rejected the same way the engine rejects an out-of-range price; the
//      normal-scenario band (~1.8k ticks wide around START_MID) never trips
//      this.
//
// The engine's coarser event vocabulary (ORDER / CANCEL / EXECUTION / a
// single generic REJECT, with no id on the reject) cannot by itself produce
// the six harness report types, so the adapter shadow-tracks
// {harness_oid -> engine_oid,side,price,remaining,alive} and synthesises
// OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject above the
// engine, echoing side/price the engine's events drop. Modify is cancel +
// reinsert (the engine has no native modify), matching the harness contract.
//
// Cgo notes: built as `package main` + buildmode=c-shared so the
// //export-tagged functions land in the produced .so as plain C symbols the
// harness's dlopen resolves directly.

package main

/*
#include <stdint.h>

// Mirror the types from api/matching_engine_api.h that the cgo bridge needs.
// We deliberately do NOT #include the harness header — cgo emits engine_*
// prototypes without the const-qualified pointer arguments the header uses,
// and the C compiler rejects the resulting type mismatch. The structs are
// byte-identical to the header (whose static_assert block fixes the layout).

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

// Go can call a C function but cannot directly invoke a function-pointer
// field of a C struct; this shim does the call.
static inline int me_push(const me_transport_t* t, void* sink,
                          const me_report_t* r) {
    return t->push(sink, r);
}
*/
import "C"

import (
	"math"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Price remap.
//
// Engine price is a uint32 index in [1, MAX_PRICE_LEVELS-1]. We place the
// workload band in the middle of that window with a fixed shift so the map is
// strictly increasing and injective (matching order is preserved exactly).
//
//   engine_price = tick - priceBase
//   tick         = engine_price + priceBase
//
// priceBase is START_MID (167.52 / 0.005 = 33504 ticks) minus the window's
// midpoint (8192), so START_MID maps to 8192. The normal band is ~32k..34k
// ticks, ~1.8k wide, landing well inside [1, 16383]. A tick that would map
// outside [1, MAX_PRICE_LEVELS-1] is treated as out of range and rejected the
// way the engine itself rejects price==0 || price>=MAX_PRICE_LEVELS.
// ---------------------------------------------------------------------------

const priceBase int64 = 33504 - 8192 // START_MID(ticks) - window midpoint

// toEnginePrice maps a harness tick to an engine price index. ok=false if the
// tick is outside the representable window.
func toEnginePrice(tick int64) (Price, bool) {
	p := tick - priceBase
	if p <= 0 || p >= MAX_PRICE_LEVELS {
		return 0, false
	}
	return Price(p), true
}

func fromEnginePrice(p Price) int64 {
	return int64(p) + priceBase
}

// ---------------------------------------------------------------------------
// Globals — single book, single matcher thread (harness contract). All state
// is touched only on the matcher thread (engine_on_* and the engine_query_*
// probes, which the harness issues from the same dispatch loop), so no
// synchronisation is needed; the report hand-off to the drainer goes through
// the harness transport.
// ---------------------------------------------------------------------------

var (
	gTransport *C.me_transport_t
	gSink      unsafe.Pointer

	gEngine *MatchingEngine

	// harness_oid -> live engine-side state.
	gShadow map[uint64]shadowEntry
	// engine_oid -> harness_oid, to translate trade maker/taker ids back.
	gEngToHarness map[uint64]uint64
)

type shadowEntry struct {
	engineOID uint64
	price     int64 // harness ticks
	side      uint8 // 0 = buy, 1 = sell
	remaining uint32
	alive     bool
}

// femto_go uses symbol 0 throughout (the harness workload is a single book).
const symbol0 Symbol = 0

// TraderID for every order: the engine only echoes it into events, which the
// adapter ignores — any value works.
const trader0 TraderID = 1

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
// Output-ring drain.
//
// RingBuffer.Read spins on an empty ring, so we cannot use it to "drain
// whatever is there". Because every engine call runs synchronously on this
// thread, the producer (the engine) and the consumer (this drain) never run
// concurrently, so we read writePos/readPos directly and pull out exactly the
// events the just-finished call produced.
//
// drainTrades is used after a new-order / modify-reinsert: the first event is
// the ORDER_EVENT (it carries the engine's freshly assigned id, which we map
// to takerHarnessOID); subsequent EXECUTION_EVENTs are fills. It returns the
// engine id of the new order and the total filled quantity, and emits one
// Trade per fill (translating maker/taker engine ids back to harness ids).
// ---------------------------------------------------------------------------

func drainTrades(seq, takerHarnessOID uint64) (engineOID uint64, filled uint32) {
	r := gEngine.outputRing
	for {
		w := r.writePos
		rd := r.readPos
		if w == rd {
			break
		}
		ev := r.buffer[rd&RING_MASK]
		r.readPos = rd + 1

		switch ev.eventType {
		case ORDER_EVENT:
			engineOID = uint64(ev.orderID)
			gEngToHarness[engineOID] = takerHarnessOID
		case EXECUTION_EVENT:
			makerEng := uint64(ev.counterOrderID)
			makerHarness, ok := gEngToHarness[makerEng]
			if !ok {
				makerHarness = makerEng // best effort; should not happen
			}
			q := uint32(ev.size)
			emitTrade(seq, makerHarness, takerHarnessOID, fromEnginePrice(ev.price), q)
			filled += q
			// Decrement the maker's shadow; retire if fully filled.
			if e, ok := gShadow[makerHarness]; ok {
				if e.remaining >= q {
					e.remaining -= q
				} else {
					e.remaining = 0
				}
				if e.remaining == 0 {
					e.alive = false
				}
				gShadow[makerHarness] = e
			}
		case REJECT_EVENT:
			// A new order the engine rejected — cannot happen here: the
			// adapter screens out-of-range prices before calling, and the
			// workload never sends qty==0 (the engine's only other reject
			// cause). Kept defensively.
		case CANCEL_EVENT:
			// Not produced by Limit; ignored if it appears.
		}
	}
	return engineOID, filled
}

// discardEvents drains and drops whatever the just-finished engine call
// produced (used for the IOC residual's engine-side Cancel, whose outcome is
// already known — the residual was just observed resting).
func discardEvents() {
	r := gEngine.outputRing
	for {
		w := r.writePos
		rd := r.readPos
		if w == rd {
			break
		}
		r.readPos = rd + 1
	}
}

// cancelOutcome drains the events of a just-issued engine Cancel and returns
// the engine's own verdict: CANCEL_EVENT = removed, REJECT_EVENT = not
// resting. The engine zeroes orderIndex[id] on every unlink — cancel and
// full fill alike — so a once-known id is always safe to ask: a stale id
// fails the engine's slot==0 self-validation and cannot alias a reused slot.
func cancelOutcome() bool {
	ok := false
	r := gEngine.outputRing
	for {
		w := r.writePos
		rd := r.readPos
		if w == rd {
			break
		}
		ev := r.buffer[rd&RING_MASK]
		r.readPos = rd + 1
		switch ev.eventType {
		case CANCEL_EVENT:
			ok = true
		case REJECT_EVENT:
			ok = false
		}
	}
	return ok
}

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

//export engine_init
func engine_init(seed C.uint64_t, transport *C.me_transport_t, report_sink unsafe.Pointer) {
	gTransport = transport
	gSink = report_sink
	gEngine = NewMatchingEngine()
	gShadow = make(map[uint64]shadowEntry, 1<<21)
	gEngToHarness = make(map[uint64]uint64, 1<<21)
}

//export engine_shutdown
func engine_shutdown() {
	gEngine = nil
	gShadow = nil
	gEngToHarness = nil
}

//export engine_flush
func engine_flush() {
	// Synchronous matcher: engine_on_* has already pushed every report and
	// drained the output ring before returning.
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) {
	seq := uint64(o.sequence_number)
	oid := uint64(o.order_id)
	side := uint8(o.side)
	tick := int64(o.price_ticks)
	qty := uint32(o.quantity)

	// OrderAck (engine fires its own ORDER_EVENT too; we consume that for
	// the id mapping but do not turn it into a report).
	emitAck(C.uint8_t(C.ME_ORDER_ACK), seq, oid, side, tick, qty)

	ep, ok := toEnginePrice(tick)
	if !ok {
		// Out of the engine's representable price window. The engine would
		// reject this; for a GTC order there is nothing resting and no fills.
		// An IOC with no fills produces a residual CancelAck.
		if o.ioc != 0 {
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, tick, qty)
		}
		return
	}

	var sideT Side
	if side == 0 {
		sideT = Bid
	} else {
		sideT = Ask
	}

	gEngine.Limit(symbol0, sideT, ep, Size(qty), trader0)
	engineOID, filled := drainTrades(seq, oid)

	var residual uint32
	if filled < qty {
		residual = qty - filled
	}

	if o.ioc != 0 {
		// IoC residual: the engine rests the unfilled remainder (it has no
		// IoC flag), so we must cancel it back out and report the residual
		// CancelAck. engineOID is 0 only if the order was rejected — the
		// screened price (handled above) or a qty==0 the workload never
		// sends; here it is always valid.
		if residual > 0 {
			gEngine.Cancel(OrderID(engineOID))
			discardEvents()
			emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, side, tick, residual)
		}
		return
	}

	// GTC: shadow tracks the resting remainder and the engine id (for a later
	// cancel / modify). Even a fully-filled order is recorded (alive=false) so
	// a later cancel/modify of it is adjudicated by the ENGINE (its
	// REJECT_EVENT -> CancelReject) rather than short-circuited as never-seen
	// — the never-seen gate fires only where no engine id exists to ask with.
	gShadow[oid] = shadowEntry{
		engineOID: engineOID,
		price:     tick,
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
	if !ok || e.engineOID == 0 {
		// No engine id recorded — the engine cannot be asked (its Cancel is
		// keyed by its own assigned ids). Never-seen harness ids land here.
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	// The engine adjudicates for every known engine id: CANCEL_EVENT on
	// removal, REJECT_EVENT for already-terminal ids.
	gEngine.Cancel(OrderID(e.engineOID))
	if !cancelOutcome() {
		emitAck(C.uint8_t(C.ME_CANCEL_REJECT), seq, oid, 0, 0, 0)
		return
	}

	emitAck(C.uint8_t(C.ME_CANCEL_ACK), seq, oid, e.side, e.price, e.remaining)
	e.alive = false // audit queries scan alive/remaining
	gShadow[oid] = e
}

//export engine_on_modify
func engine_on_modify(m *C.modify_t) {
	seq := uint64(m.sequence_number)
	oid := uint64(m.order_id)
	newTick := int64(m.new_price_ticks)
	newQty := uint32(m.new_quantity)

	e, ok := gShadow[oid]
	if !ok || e.engineOID == 0 {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}
	side := e.side

	// The engine adjudicates the cancel half of cancel + reinsert.
	gEngine.Cancel(OrderID(e.engineOID))
	if !cancelOutcome() {
		emitAck(C.uint8_t(C.ME_MODIFY_REJECT), seq, oid, 0, 0, 0)
		return
	}

	// One ModifyAck for the reprice/resize.
	emitAck(C.uint8_t(C.ME_MODIFY_ACK), seq, oid, side, newTick, newQty)

	ep, okp := toEnginePrice(newTick)
	if !okp {
		// New price out of the engine's window: nothing rests, no fills.
		gShadow[oid] = shadowEntry{
			price:     newTick,
			side:      side,
			remaining: newQty,
			alive:     false, // not representable in the book; treat as gone
		}
		return
	}

	var sideT Side
	if side == 0 {
		sideT = Bid
	} else {
		sideT = Ask
	}
	gEngine.Limit(symbol0, sideT, ep, Size(newQty), trader0)
	engineOID, filled := drainTrades(seq, oid)

	var residual uint32
	if filled < newQty {
		residual = newQty - filled
	}
	gShadow[oid] = shadowEntry{
		engineOID: engineOID,
		price:     newTick,
		side:      side,
		remaining: residual,
		alive:     residual > 0,
	}
}

// ---------------------------------------------------------------------------
// Audit queries.
//
// Read from the shadow map (the engine exposes best bid/ask only as the
// bidMax/askMin price indices on its book, but the shadow is the source of
// truth that already carries harness ticks and live remainders). O(N) scans,
// but audit queries are rare.
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
