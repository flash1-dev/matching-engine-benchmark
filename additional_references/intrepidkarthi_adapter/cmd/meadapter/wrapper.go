// Command meadapter exposes intrepidkarthi/orderbook's matching engine through the
// benchmark harness's C ABI, api/matching_engine_api.h.
//
//	go build -buildmode=c-shared -o intrepidkarthi_adapter.so ./cmd/meadapter
//
// The logic lives in internal/meadapter; this file only moves messages and reports
// across the boundary.
package main

/*
#include <stddef.h>
#include <stdint.h>

// The harness's structs, declared from the header's documented layout rather than
// included: the header is not vendored upstream, and cgo's prototypes
// for the exports would not match the header's const-qualified ones anyway. The
// asserts pin every size and the offsets the adapter reads or writes; a mismatch with
// the real header fails the harness's correctness hash on CI.

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

// One batch element: a tag, then the message at offset 8.
typedef struct {
	uint8_t tag;
	uint8_t _pad[7];
	uint8_t payload[32];
} me_msg_t;

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

_Static_assert(sizeof(new_order_t) == 32, "new_order_t");
_Static_assert(offsetof(new_order_t, quantity) == 24, "new_order_t.quantity");
_Static_assert(offsetof(new_order_t, side) == 28, "new_order_t.side");
_Static_assert(offsetof(new_order_t, ioc) == 29, "new_order_t.ioc");
_Static_assert(sizeof(cancel_t) == 16, "cancel_t");
_Static_assert(sizeof(modify_t) == 32, "modify_t");
_Static_assert(offsetof(modify_t, side) == 28, "modify_t.side");
_Static_assert(sizeof(me_msg_t) == 40, "me_msg_t");
_Static_assert(offsetof(me_msg_t, payload) == 8, "me_msg_t.payload");
_Static_assert(sizeof(me_report_t) == 64, "me_report_t");
_Static_assert(offsetof(me_report_t, sequence_number) == 8, "me_report_t.sequence_number");
_Static_assert(offsetof(me_report_t, quantity) == 32, "me_report_t.quantity");
_Static_assert(offsetof(me_report_t, maker_order_id) == 40, "me_report_t.maker_order_id");

// Go cannot call through a C function pointer. This hands a run of reports over in
// one crossing; push returns 0 while the transport is full, so it retries.
static inline void me_push_n(const me_transport_t* t, void* sink, const me_report_t* r, uint32_t n) {
	for (uint32_t i = 0; i < n; i++)
		while (!t->push(sink, &r[i])) { }
}
*/
import "C"

import (
	"unsafe"

	"github.com/intrepidkarthi/orderbook/internal/meadapter"
)

func main() {}

// The harness has one matcher thread, so these globals have one writer.
var (
	transport *C.me_transport_t
	sink      unsafe.Pointer
	adapter   *meadapter.Adapter
	// out holds the reports of the call or batch in progress. Its backing array is
	// reused, so emitting does not allocate once it has grown.
	out []C.me_report_t
)

func emit(r meadapter.Report) {
	out = append(out, C.me_report_t{
		_type:           C.uint8_t(r.Type),
		side:            C.uint8_t(r.Side),
		sequence_number: C.uint64_t(r.Seq),
		order_id:        C.uint64_t(r.OrderID),
		price_ticks:     C.int64_t(r.Price),
		quantity:        C.uint32_t(r.Qty),
		maker_order_id:  C.uint64_t(r.Maker),
		taker_order_id:  C.uint64_t(r.Taker),
	})
}

// push hands the buffered reports to the transport.
func push() {
	if len(out) == 0 {
		return
	}
	C.me_push_n(transport, sink, &out[0], C.uint32_t(len(out)))
	out = out[:0]
}

//export engine_init
func engine_init(seed C.uint64_t, t *C.me_transport_t, reportSink unsafe.Pointer) {
	transport, sink = t, reportSink
	out = make([]C.me_report_t, 0, 1<<12)
	adapter = meadapter.New(emit)
}

//export engine_shutdown
func engine_shutdown() {
	adapter, transport, sink, out = nil, nil, nil, nil
}

//export engine_flush
func engine_flush() { push() }

func newOrder(o *C.new_order_t) {
	adapter.NewOrder(uint64(o.sequence_number), uint64(o.order_id), int64(o.price_ticks),
		uint32(o.quantity), uint8(o.side), o.ioc != 0)
}

func cancel(c *C.cancel_t) {
	adapter.Cancel(uint64(c.sequence_number), uint64(c.order_id))
}

func modify(m *C.modify_t) {
	adapter.Modify(uint64(m.sequence_number), uint64(m.order_id), int64(m.new_price_ticks),
		uint32(m.new_quantity), uint8(m.side))
}

//export engine_on_new_order
func engine_on_new_order(o *C.new_order_t) { newOrder(o); push() }

//export engine_on_cancel
func engine_on_cancel(c *C.cancel_t) { cancel(c); push() }

//export engine_on_modify
func engine_on_modify(m *C.modify_t) { modify(m); push() }

// engine_on_batch takes a run of messages in one crossing, and returns its reports in
// one crossing. The order of reports is the same as one message at a time.
//
//export engine_on_batch
func engine_on_batch(msgs *C.me_msg_t, n C.uint32_t) {
	ms := unsafe.Slice(msgs, int(n))
	for i := range ms {
		m := &ms[i]
		p := unsafe.Pointer(&m.payload[0])
		switch m.tag {
		case 0:
			newOrder((*C.new_order_t)(p))
		case 1:
			cancel((*C.cancel_t)(p))
		case 2:
			modify((*C.modify_t)(p))
		}
	}
	push()
}

//export engine_query_best_bid
func engine_query_best_bid() C.int64_t { return C.int64_t(adapter.BestBid()) }

//export engine_query_best_ask
func engine_query_best_ask() C.int64_t { return C.int64_t(adapter.BestAsk()) }

//export engine_query_depth_at
func engine_query_depth_at(price C.int64_t, side C.uint8_t) C.uint64_t {
	return C.uint64_t(adapter.DepthAt(int64(price), uint8(side)))
}
