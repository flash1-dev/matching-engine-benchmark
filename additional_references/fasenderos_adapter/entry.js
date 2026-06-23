// Adapter entry shim for the matching-engine benchmark harness.
//
// Bundled (with the engine + its pure-JS deps) into a single self-contained
// IIFE that is evaluated inside an embedded V8 isolate by fasenderos_adapter.so.
// It exposes a flat, primitive-only API on globalThis.LOB so the C++ side can
// drive one message per call and read back a compact result with no per-call
// JS object churn on the boundary.
//
// Report model (matches docs/INTEGRATION.md):
//   - Trades are captured at the single fill site in OrderBook.processQueue via
//     a global hook (__ME_onFill) that the engine source is patched to call,
//     exactly like the jxm35 notify_trade injection. Each fill carries the
//     maker's resting price, the maker id, the taker id, and the fill quantity,
//     emitted in match order. This is the engine's native per-fill event; the
//     adapter does not reconstruct fills from the IProcessOrder summary.
//   - OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject are derived
//     from the OrderBook return values (err codes / undefined) per call.

import { OrderBook } from "../src/orderbook.js";
import { Side, TimeInForce, OrderType } from "../src/types.js";

// ---- trade capture ----------------------------------------------------------
// A flat ring of fills produced by the current message. The C++ side reads
// these out after each engine_on_* call. Layout per fill: [makerId, takerId,
// priceTicks, qty]. ids are integers (harness order ids fit in 32 bits; we keep
// them as JS numbers, exact up to 2^53).
let fillMaker = new Float64Array(0);
let fillTaker = new Float64Array(0);
let fillPrice = new Float64Array(0);
let fillQty = new Float64Array(0);
let fillN = 0;
let fillCap = 0;

function ensureFillCap(n) {
	if (n <= fillCap) return;
	let c = fillCap === 0 ? 64 : fillCap;
	while (c < n) c <<= 1;
	const m = new Float64Array(c);
	const t = new Float64Array(c);
	const p = new Float64Array(c);
	const q = new Float64Array(c);
	m.set(fillMaker);
	t.set(fillTaker);
	p.set(fillPrice);
	q.set(fillQty);
	fillMaker = m;
	fillTaker = t;
	fillPrice = p;
	fillQty = q;
	fillCap = c;
}

// The taker id of the message currently being processed. Set before each call.
let curTaker = 0;

// Global hook the patched engine calls once per fill, in match order.
//   makerId : id string of the resting (maker) order being consumed
//   price   : maker's resting price (ticks)
//   qty     : quantity filled against that maker in this step
globalThis.__ME_onFill = (makerId, price, qty) => {
	ensureFillCap(fillN + 1);
	fillMaker[fillN] = +makerId;
	fillTaker[fillN] = curTaker;
	fillPrice[fillN] = price;
	fillQty[fillN] = qty;
	fillN++;
};

// ---- engine instance --------------------------------------------------------
let book = new OrderBook();

// Result of the last call, read by C++:
//   [0] status: 0 = ack (order accepted / cancel ok / modify ok)
//              1 = reject (cancel/modify of a non-resting order)
//   [1] fillCount: fills appended to the fill ring for this message.
//   [2] side  of the order this report concerns (0 buy / 1 sell), where known.
//   [3] price (ticks) of the order this report concerns, where known.
const result = new Int32Array(2);
// side/price are reported as doubles so a 53-bit-safe price round-trips.
const meta = new Float64Array(2); // [side, price]

function sideOf(s) {
	return s === 0 ? Side.BUY : Side.SELL;
}

// ---- public API -------------------------------------------------------------
globalThis.LOB = {
	reset() {
		book = new OrderBook();
		fillN = 0;
	},

	// new order. returns nothing; read LOB result/fills after.
	newOrder(id, side, price, qty, ioc) {
		fillN = 0;
		curTaker = id;
		if (ioc) {
			book.limit({
				type: OrderType.LIMIT,
				id: String(id),
				side: sideOf(side),
				size: qty,
				price: price,
				timeInForce: TimeInForce.IOC,
			});
		} else {
			book.limit({
				type: OrderType.LIMIT,
				id: String(id),
				side: sideOf(side),
				size: qty,
				price: price,
			});
		}
		// A new order is always an OrderAck (status 0). IOC residual handling
		// (CancelAck) is decided on the C++ side from whether the order is still
		// resting after the call.
		result[0] = 0;
		result[1] = fillN;
		return fillN;
	},

	cancel(id) {
		fillN = 0;
		const r = book.cancel(String(id));
		// cancel() returns undefined if the order was not resting.
		// It can also return an object whose .order is undefined in some engine
		// paths; treat a missing resting order as a reject.
		if (r !== undefined && r.order !== undefined) {
			result[0] = 0;
			meta[0] = r.order.side === Side.BUY ? 0 : 1;
			meta[1] = r.order.price;
		} else {
			result[0] = 1;
		}
		result[1] = 0;
		return 0;
	},

	modify(id, newPrice, newSize) {
		fillN = 0;
		curTaker = id;
		const r = book.modify(String(id), { price: newPrice, size: newSize });
		// modify() sets err (ORDER_NOT_FOUND) when the order is not resting.
		result[0] = r.err === null ? 0 : 1;
		result[1] = fillN;
		return fillN;
	},

	// Queries.
	bestBid() {
		const [, bids] = book.depth();
		// depth() bids are sorted best-first (price tree ordered desc for bids).
		return bids.length > 0 ? bids[0][0] : null;
	},
	bestAsk() {
		const [asks] = book.depth();
		return asks.length > 0 ? asks[0][0] : null;
	},
	depthAt(price, side) {
		const [asks, bids] = book.depth();
		const arr = side === 0 ? bids : asks;
		for (let i = 0; i < arr.length; i++) {
			if (arr[i][0] === price) return arr[i][1];
		}
		return 0;
	},
	// Is an order currently resting? (used to decide IOC residual CancelAck)
	isResting(id) {
		return book.order(String(id)) !== undefined ? 1 : 0;
	},

	// Accessors for the last result + fill ring (returned as typed arrays so
	// C++ reads the backing store directly). These buffers are fetched once at
	// init and read directly thereafter — no per-message JS calls for status.
	resultBuf() {
		return result; // [status, fillCount]
	},
	metaBuf() {
		return meta; // [side, price] of the order a cancel/modify ack concerns
	},
	status() {
		return result[0];
	},
	fillCount() {
		return fillN;
	},
	fillMakerBuf() {
		return fillMaker;
	},
	fillTakerBuf() {
		return fillTaker;
	},
	fillPriceBuf() {
		return fillPrice;
	},
	fillQtyBuf() {
		return fillQty;
	},
};
