// fasenderos_adapter.cpp — harness adapter for fasenderos/nodejs-order-book.
//
// The engine is TypeScript/Node.js. This adapter embeds V8 (the JS engine inside
// the system libnode) in-process: at engine_init() it spins up a V8 isolate,
// evaluates the bundled engine JS (engine + denque + functional-red-black-tree,
// produced by build.sh), and caches handles to the flat globalThis.LOB API. Each
// harness hot-path call invokes the corresponding LOB method synchronously on the
// calling thread and turns the result + per-fill trade events into the report
// stream. No Node event loop, no out-of-process server.
//
// Report mapping (docs/INTEGRATION.md):
//   new order  -> OrderAck, then one Trade per fill (maker price, maker/taker
//                 ids, fill qty, in match order); IOC residual -> CancelAck.
//   cancel     -> CancelAck if it was resting, else CancelReject.
//   modify     -> Trades for any crossing fills then ModifyAck if resting, else
//                 ModifyReject. (Engine does cancel+reinsert natively.)

#include "matching_engine_api.h"
#include "bundle_js.h"

#include <v8.h>
#include <libplatform/libplatform.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace v8;

namespace {

// ---- transport --------------------------------------------------------------
const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

inline void push_report(const me_report_t& r) {
	while (!g_transport->push(g_sink, &r)) { /* spin: queue full */ }
}

// ---- V8 state ---------------------------------------------------------------
// All V8 persistent handles and the platform live inside a single heap-allocated
// struct that is intentionally LEAKED (never deleted). This guarantees no V8
// destructor runs at process exit: tearing an isolate down from a static C++
// destructor (or from engine_shutdown) races libnode's own atexit handlers and
// segfaults in GlobalHandles::Destroy. Leaking is the correct, safe choice for a
// one-shot benchmark process.
struct V8State {
	Platform*                platform = nullptr;
	Isolate*                 isolate  = nullptr;
	ArrayBuffer::Allocator*  alloc    = nullptr;
	Global<Context>          context;

	Global<Object>   lob;
	Global<Function> fn_newOrder, fn_cancel, fn_modify;
	Global<Function> fn_bestBid, fn_bestAsk, fn_depthAt, fn_isResting;
	Global<Function> fn_fillMaker, fn_fillTaker, fn_fillPrice, fn_fillQty;

	// Direct pointers into the fixed-size result/meta backing stores (never
	// reallocated), fetched once at init.
	//   result[0] = status (0 ack / 1 reject), result[1] = fill count
	//   meta[0]   = side of the acked cancel/modify order, meta[1] = its price
	int32_t* result = nullptr;
	double*  meta   = nullptr;
	Global<Int32Array>   resultArr;
	Global<Float64Array> metaArr;
};
V8State* g_st = nullptr;

inline Local<String> s(Isolate* iso, const char* str) {
	return String::NewFromUtf8(iso, str, NewStringType::kNormal).ToLocalChecked();
}

Local<Function> get_method(Isolate* iso, Local<Context> ctx, Local<Object> obj,
                           const char* name) {
	Local<Value> v = obj->Get(ctx, s(iso, name)).ToLocalChecked();
	if (!v->IsFunction()) {
		fprintf(stderr, "[fasenderos] LOB.%s is not a function\n", name);
		abort();
	}
	return v.As<Function>();
}

// Pull the fill arrays for the just-processed message and emit Trades.
// `n` fills, taker sequence_number = seq, taker order id = taker_oid.
void emit_fills(Isolate* iso, Local<Context> ctx, uint32_t n, uint64_t seq,
                uint64_t taker_oid, uint8_t side, uint64_t* filled_out) {
	uint64_t filled = 0;
	if (n == 0) { if (filled_out) *filled_out = 0; return; }

	Local<Function> fM = g_st->fn_fillMaker.Get(iso);
	Local<Function> fP = g_st->fn_fillPrice.Get(iso);
	Local<Function> fQ = g_st->fn_fillQty.Get(iso);
	Local<Object> lob = g_st->lob.Get(iso);

	Local<Value> aM = fM->Call(ctx, lob, 0, nullptr).ToLocalChecked();
	Local<Value> aP = fP->Call(ctx, lob, 0, nullptr).ToLocalChecked();
	Local<Value> aQ = fQ->Call(ctx, lob, 0, nullptr).ToLocalChecked();

	Local<Float64Array> bM = aM.As<Float64Array>();
	Local<Float64Array> bP = aP.As<Float64Array>();
	Local<Float64Array> bQ = aQ.As<Float64Array>();

	// Direct access to the backing store (no per-element JS calls).
	double* pM = static_cast<double*>(bM->Buffer()->GetBackingStore()->Data())
	             + bM->ByteOffset() / sizeof(double);
	double* pP = static_cast<double*>(bP->Buffer()->GetBackingStore()->Data())
	             + bP->ByteOffset() / sizeof(double);
	double* pQ = static_cast<double*>(bQ->Buffer()->GetBackingStore()->Data())
	             + bQ->ByteOffset() / sizeof(double);

	for (uint32_t i = 0; i < n; i++) {
		me_report_t r;
		memset(&r, 0, sizeof(r));
		r.type            = ME_TRADE;
		r.side            = side;
		r.sequence_number = seq;
		r.order_id        = taker_oid;
		r.price_ticks     = static_cast<int64_t>(pP[i]);
		uint32_t q        = static_cast<uint32_t>(pQ[i]);
		r.quantity        = q;
		r.maker_order_id  = static_cast<uint64_t>(pM[i]);
		r.taker_order_id  = taker_oid;
		push_report(r);
		filled += q;
	}
	if (filled_out) *filled_out = filled;
}

} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
extern "C" void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                            void* report_sink) {
	g_transport = transport;
	g_sink      = report_sink;

	g_st = new V8State();   // intentionally leaked (see V8State comment)

	// One-time V8 platform init.
	g_st->platform = platform::NewDefaultPlatform().release();
	V8::InitializePlatform(g_st->platform);
	V8::Initialize();

	Isolate::CreateParams params;
	g_st->alloc = ArrayBuffer::Allocator::NewDefaultAllocator();
	params.array_buffer_allocator = g_st->alloc;
	Isolate* iso = Isolate::New(params);
	g_st->isolate = iso;

	Isolate::Scope iscope(iso);
	HandleScope hs(iso);
	Local<Context> ctx = Context::New(iso);
	g_st->context.Reset(iso, ctx);
	Context::Scope cscope(ctx);

	// Evaluate the engine bundle (defines globalThis.LOB and __ME_onFill).
	Local<String> src = String::NewFromUtf8(iso, ENGINE_BUNDLE_JS,
	                                         NewStringType::kNormal).ToLocalChecked();
	TryCatch tc(iso);
	Local<Script> script;
	if (!Script::Compile(ctx, src).ToLocal(&script)) {
		String::Utf8Value e(iso, tc.Exception());
		fprintf(stderr, "[fasenderos] bundle compile error: %s\n", *e);
		abort();
	}
	if (script->Run(ctx).IsEmpty()) {
		String::Utf8Value e(iso, tc.Exception());
		fprintf(stderr, "[fasenderos] bundle run error: %s\n", *e);
		abort();
	}

	Local<Value> lobv = ctx->Global()->Get(ctx, s(iso, "LOB")).ToLocalChecked();
	if (!lobv->IsObject()) { fprintf(stderr, "[fasenderos] LOB missing\n"); abort(); }
	Local<Object> lob = lobv.As<Object>();
	g_st->lob.Reset(iso, lob);

	g_st->fn_newOrder.Reset(iso, get_method(iso, ctx, lob, "newOrder"));
	g_st->fn_cancel.Reset(iso,   get_method(iso, ctx, lob, "cancel"));
	g_st->fn_modify.Reset(iso,   get_method(iso, ctx, lob, "modify"));
	g_st->fn_bestBid.Reset(iso,  get_method(iso, ctx, lob, "bestBid"));
	g_st->fn_bestAsk.Reset(iso,  get_method(iso, ctx, lob, "bestAsk"));
	g_st->fn_depthAt.Reset(iso,  get_method(iso, ctx, lob, "depthAt"));
	g_st->fn_isResting.Reset(iso, get_method(iso, ctx, lob, "isResting"));
	g_st->fn_fillMaker.Reset(iso, get_method(iso, ctx, lob, "fillMakerBuf"));
	g_st->fn_fillTaker.Reset(iso, get_method(iso, ctx, lob, "fillTakerBuf"));
	g_st->fn_fillPrice.Reset(iso, get_method(iso, ctx, lob, "fillPriceBuf"));
	g_st->fn_fillQty.Reset(iso,  get_method(iso, ctx, lob, "fillQtyBuf"));

	// Fetch the fixed-size result/meta backing stores once and cache raw pointers.
	{
		Local<Function> rfn = get_method(iso, ctx, lob, "resultBuf");
		Local<Value> rv = rfn->Call(ctx, lob, 0, nullptr).ToLocalChecked();
		Local<Int32Array> ra = rv.As<Int32Array>();
		g_st->resultArr.Reset(iso, ra);
		g_st->result = static_cast<int32_t*>(ra->Buffer()->GetBackingStore()->Data())
		               + ra->ByteOffset() / sizeof(int32_t);

		Local<Function> mfn = get_method(iso, ctx, lob, "metaBuf");
		Local<Value> mv = mfn->Call(ctx, lob, 0, nullptr).ToLocalChecked();
		Local<Float64Array> ma = mv.As<Float64Array>();
		g_st->metaArr.Reset(iso, ma);
		g_st->meta = static_cast<double*>(ma->Buffer()->GetBackingStore()->Data())
		             + ma->ByteOffset() / sizeof(double);
	}
}

extern "C" void engine_shutdown(void) {
	// Intentionally a near no-op: V8State is leaked so no V8 teardown runs at
	// process exit (disposing the isolate here races libnode's atexit handlers
	// and crashes in GlobalHandles::Destroy). The OS reclaims everything.
}

// Enter the isolate + persistent context for one hot-path / query call.
#define ME_ENTER()                                  \
	Isolate* iso = g_st->isolate;                   \
	Isolate::Scope iscope(iso);                     \
	HandleScope hs(iso);                            \
	Local<Context> ctx = g_st->context.Get(iso);    \
	Context::Scope cscope(ctx);                     \
	Local<Object> lob = g_st->lob.Get(iso)

// ===========================================================================
// Hot path
// ===========================================================================
extern "C" void engine_on_new_order(const new_order_t* o) {
	ME_ENTER();

	// OrderAck first.
	{
		me_report_t r; memset(&r, 0, sizeof(r));
		r.type = ME_ORDER_ACK; r.side = o->side;
		r.sequence_number = o->sequence_number; r.order_id = o->order_id;
		r.price_ticks = o->price_ticks; r.quantity = o->quantity;
		push_report(r);
	}

	Local<Value> argv[5] = {
		Number::New(iso, static_cast<double>(o->order_id)),
		Integer::New(iso, o->side),
		Number::New(iso, static_cast<double>(o->price_ticks)),
		Number::New(iso, static_cast<double>(o->quantity)),
		Integer::New(iso, o->ioc ? 1 : 0),
	};
	Local<Value> rv = g_st->fn_newOrder.Get(iso)->Call(ctx, lob, 5, argv).ToLocalChecked();
	uint32_t n = static_cast<uint32_t>(rv->Uint32Value(ctx).FromMaybe(0));

	uint64_t filled = 0;
	emit_fills(iso, ctx, n, o->sequence_number, o->order_id, o->side, &filled);

	// IOC residual -> CancelAck for the unfilled remainder.
	if (o->ioc && filled < o->quantity) {
		me_report_t r; memset(&r, 0, sizeof(r));
		r.type = ME_CANCEL_ACK; r.side = o->side;
		r.sequence_number = o->sequence_number; r.order_id = o->order_id;
		r.price_ticks = o->price_ticks;
		r.quantity = static_cast<uint32_t>(o->quantity - filled);
		push_report(r);
	}
}

extern "C" void engine_on_cancel(const cancel_t* c) {
	ME_ENTER();

	Local<Value> argv[1] = { Number::New(iso, static_cast<double>(c->order_id)) };
	g_st->fn_cancel.Get(iso)->Call(ctx, lob, 1, argv).ToLocalChecked();
	bool ok = g_st->result[0] == 0;

	me_report_t r; memset(&r, 0, sizeof(r));
	r.sequence_number = c->sequence_number; r.order_id = c->order_id;
	if (ok) {
		// CancelAck carries the canceled order's side + resting price.
		r.type = ME_CANCEL_ACK;
		r.side = static_cast<uint8_t>(g_st->meta[0]);
		r.price_ticks = static_cast<int64_t>(g_st->meta[1]);
	} else {
		// CancelReject carries only seq + order_id.
		r.type = ME_CANCEL_REJECT;
	}
	push_report(r);
}

extern "C" void engine_on_modify(const modify_t* m) {
	ME_ENTER();

	Local<Value> argv[3] = {
		Number::New(iso, static_cast<double>(m->order_id)),
		Number::New(iso, static_cast<double>(m->new_price_ticks)),
		Number::New(iso, static_cast<double>(m->new_quantity)),
	};
	Local<Value> rv = g_st->fn_modify.Get(iso)->Call(ctx, lob, 3, argv).ToLocalChecked();
	uint32_t n = static_cast<uint32_t>(rv->Uint32Value(ctx).FromMaybe(0));
	bool ok = g_st->result[0] == 0;

	if (ok) {
		// Crossing fills produced by the reinsert.
		emit_fills(iso, ctx, n, m->sequence_number, m->order_id, m->side, nullptr);
		me_report_t r; memset(&r, 0, sizeof(r));
		r.type = ME_MODIFY_ACK; r.side = m->side;
		r.sequence_number = m->sequence_number; r.order_id = m->order_id;
		r.price_ticks = m->new_price_ticks; r.quantity = m->new_quantity;
		push_report(r);
	} else {
		me_report_t r; memset(&r, 0, sizeof(r));
		r.type = ME_MODIFY_REJECT; r.side = m->side;
		r.sequence_number = m->sequence_number; r.order_id = m->order_id;
		push_report(r);
	}
}

extern "C" void engine_flush(void) { /* synchronous engine: no-op */ }

// ===========================================================================
// Queries
// ===========================================================================
extern "C" int64_t engine_query_best_bid(void) {
	ME_ENTER();
	Local<Value> rv = g_st->fn_bestBid.Get(iso)->Call(ctx, lob, 0, nullptr).ToLocalChecked();
	if (rv->IsNull() || rv->IsUndefined()) return INT64_MIN;
	return static_cast<int64_t>(rv->NumberValue(ctx).FromMaybe(0));
}

extern "C" int64_t engine_query_best_ask(void) {
	ME_ENTER();
	Local<Value> rv = g_st->fn_bestAsk.Get(iso)->Call(ctx, lob, 0, nullptr).ToLocalChecked();
	if (rv->IsNull() || rv->IsUndefined()) return INT64_MAX;
	return static_cast<int64_t>(rv->NumberValue(ctx).FromMaybe(0));
}

extern "C" uint64_t engine_query_depth_at(int64_t price, uint8_t side) {
	ME_ENTER();
	Local<Value> argv[2] = {
		Number::New(iso, static_cast<double>(price)),
		Integer::New(iso, side),
	};
	Local<Value> rv = g_st->fn_depthAt.Get(iso)->Call(ctx, lob, 2, argv).ToLocalChecked();
	return static_cast<uint64_t>(rv->NumberValue(ctx).FromMaybe(0));
}
