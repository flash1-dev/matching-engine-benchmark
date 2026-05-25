/*
 * matching_engine_api.h — the matching-engine benchmark integration contract.
 *
 * Published with "The World's Fastest Matching Engine Algorithm"
 * (Flash One Technologies, 2026).
 *
 * An engine under test is built as a shared library (.so) that exports the
 * symbols below. The harness loads it with dlopen() and drives it one message
 * at a time.
 *
 * ----------------------------------------------------------------------------
 * Contract requirements
 * ----------------------------------------------------------------------------
 *  - The engine emits its own report stream. For every message the engine
 *    produces the reports it generates — OrderAck, Trade, CancelAck (including
 *    the cancellation of an IOC residual), ModifyAck, and CancelReject /
 *    ModifyReject for a cancel or modify of an order that is not resting — and
 *    pushes each into the report transport (see "Report transport" below). The
 *    harness does not synthesize reports; it only drains the stream the engine
 *    produces.
 *  - The engine may use threads. It can match on the calling thread or on its
 *    own thread(s), and report from whichever thread it likes — whatever
 *    reflects how it runs in production. There is no single-thread restriction.
 *  - engine_flush() is the pipeline barrier. After the harness has delivered
 *    the last message it calls engine_flush(); the call must not return until
 *    every message delivered so far has been fully matched and every resulting
 *    report has been pushed into the transport. engine_flush() is inside the
 *    measured window, so deferred work is always counted — an engine cannot
 *    appear fast by returning early and finishing after the clock stops.
 *  - engine_query_* must reflect every message delivered before the query call
 *    returns. For an engine that matches asynchronously the query is a
 *    synchronization point: it must observe all preceding messages.
 *  - Prices are signed integer ticks; one tick is $0.005 (SEC sub-penny rule).
 *  - Each Trade report carries the aggressive (incoming) order's
 *    sequence_number; the fill price is the maker's (resting order's) price.
 *
 * Modify is handled as cancel + reinsert: the order is removed and re-added at
 * its new price/quantity, losing queue (time) priority. Every modify in the
 * canonical workload is a reprice or a quantity increase, which production
 * exchanges handle exactly this way.
 *
 * See docs/INTEGRATION.md for a worked walkthrough and docs/METHODOLOGY.md for
 * the measurement protocol.
 */

#ifndef MATCHING_ENGINE_API_H
#define MATCHING_ENGINE_API_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Order types — one per workload message
 * ===========================================================================*/

typedef struct {
    uint64_t order_id;
    uint64_t sequence_number;
    int64_t  price_ticks;        /* signed integer ticks, $0.005 per tick   */
    uint32_t quantity;
    uint8_t  side;               /* 0 = buy, 1 = sell                       */
    uint8_t  ioc;                /* 1 if immediate-or-cancel                */
    uint8_t  _reserved[2];       /* pad to 32 bytes                         */
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
    uint8_t  side;               /* 0 = buy, 1 = sell — the order's side    */
    uint8_t  _reserved[3];       /* pad to 32 bytes                         */
} modify_t;

/* ===========================================================================
 * Report stream
 *
 * The engine emits six kinds of report. A production matching engine hands
 * every result to an outbound publisher across a thread boundary; the harness
 * models that hand-off with the inter-thread transport below, and the cost of
 * emitting reports is inside the measured window.
 *
 *   OrderAck      one per accepted new order,
 *   Trade         one per fill,
 *   CancelAck     one per successful cancel, and one per IOC residual
 *                 (the unfilled remainder of an immediate-or-cancel order),
 *   ModifyAck     one per successful modify,
 *   CancelReject  one per cancel of an order that is not resting — already
 *                 filled, already cancelled, or never seen (the production
 *                 "too late to cancel" / "unknown order" response),
 *   ModifyReject  one per modify of an order that is not resting.
 * ===========================================================================*/

typedef enum {
    ME_ORDER_ACK     = 0,
    ME_TRADE         = 1,
    ME_CANCEL_ACK    = 2,
    ME_MODIFY_ACK    = 3,
    ME_CANCEL_REJECT = 4,   /* cancel of an order that is not resting */
    ME_MODIFY_REJECT = 5    /* modify of an order that is not resting */
} me_report_type_t;

typedef struct {
    uint8_t  type;               /* me_report_type_t                        */
    uint8_t  side;               /* originating order side, where meaningful*/
    uint8_t  _reserved[6];
    uint64_t sequence_number;    /* aggressive / originating order          */
    uint64_t order_id;           /* order this report concerns              */
    int64_t  price_ticks;        /* ME_TRADE: the maker's resting price     */
    uint32_t quantity;
    uint32_t _reserved2;
    uint64_t maker_order_id;     /* ME_TRADE only                           */
    uint64_t taker_order_id;     /* ME_TRADE only                           */
    uint64_t _reserved3;         /* pad to a 64-byte cache line             */
} me_report_t;

/* ---------------------------------------------------------------------------
 * Report transport — the inter-thread queue the report stream travels over.
 *
 * The engine pushes reports in; a drainer thread on an adjacent core pulls
 * them out. By default the harness supplies its own single-producer /
 * single-consumer queue and hands the engine the vtable + handle through
 * engine_init(). An engine MAY instead export engine_get_transport() to
 * substitute its own queue (e.g. a proprietary lock-free ring). The transport
 * choice never affects correctness — only how reports are carried between the
 * matcher and the drainer.
 * ---------------------------------------------------------------------------*/

typedef struct {
    /* Create a transport with room for at least `capacity` reports.
     * Returns an opaque handle, or NULL on failure. */
    void*    (*create)(uint32_t capacity);
    /* Producer side: enqueue one report. Returns 1 on success, 0 if full. */
    int      (*push)(void* handle, const me_report_t* report);
    /* Consumer side (drainer thread only): dequeue up to `max` reports.
     * Returns the number dequeued (0 if empty). */
    uint32_t (*drain)(void* handle, me_report_t* out, uint32_t max);
    /* Producer side: publish any writes the transport has buffered but not yet
     * made visible to the consumer. The harness calls this once, after
     * engine_flush(), so a transport that batches its writes does not strand a
     * partial last batch. A transport whose push() publishes immediately
     * implements this as a no-op. */
    void     (*flush)(void* handle);
    /* Destroy a transport created by create(). */
    void     (*destroy)(void* handle);
} me_transport_t;

/* ===========================================================================
 * Engine lifecycle
 * ===========================================================================*/

/* Initialize the engine. Called once before any message.
 *  - seed         supplied at runtime to prevent hardcoding workload behaviour.
 *  - transport    the report transport vtable the harness has selected (the
 *                 harness default, or the engine's own from
 *                 engine_get_transport()).
 *  - report_sink  the transport handle the engine pushes reports into:
 *                 transport->push(report_sink, &report).
 * An engine that produces reports through its own internal queue (the one it
 * returned from engine_get_transport()) may ignore both arguments. */
void engine_init(uint64_t seed, const me_transport_t* transport,
                 void* report_sink);

/* Tear down the engine. Called once after all operations. */
void engine_shutdown(void);

/* ===========================================================================
 * Hot path — one call per workload message
 *
 * Each call processes one message and emits the resulting reports into the
 * transport. The calls return nothing: the engine is free to match and report
 * on whatever thread(s) it chooses, provided engine_flush() and the
 * engine_query_* calls observe the results (see the contract notes above).
 * ===========================================================================*/

/* Process a new order: match it, emit one OrderAck, one Trade per fill, and —
 * if it is an IOC order with an unfilled remainder — one CancelAck. */
void engine_on_new_order(const new_order_t* order);

/* Process a cancel: if the order is resting, remove it and emit one CancelAck;
 * otherwise — already filled, already cancelled, or never seen — emit one
 * CancelReject. */
void engine_on_cancel(const cancel_t* cancel);

/* Process a modify, handled as cancel + reinsert: if the order is resting,
 * remove it, re-add it at the new price/quantity (losing queue priority), emit
 * one Trade per fill the reinsert crosses, and one ModifyAck; otherwise emit
 * one ModifyReject. */
void engine_on_modify(const modify_t* modify);

/* Pipeline barrier. The harness calls this once, after the last message and
 * before it stops the clock. It must not return until every message delivered
 * so far has been fully matched and every resulting report has been pushed
 * into the transport. A fully synchronous engine implements this as a no-op. */
void engine_flush(void);

/* ===========================================================================
 * Audit queries — called at unpredictable points (see docs/ANTI_CHEAT.md)
 *
 * Each must reflect every message delivered before the call returns.
 * ===========================================================================*/

/* Best (highest) bid price in ticks, or INT64_MIN if there are no bids. */
int64_t engine_query_best_bid(void);

/* Best (lowest) ask price in ticks, or INT64_MAX if there are no asks. */
int64_t engine_query_best_ask(void);

/* Aggregated resting quantity at one price level (0 if the level is empty).
 * side: 0 = buy, 1 = sell. */
uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side);

/* ===========================================================================
 * OPTIONAL: engine-supplied report transport
 *
 * Export this to make the harness carry the report stream over the engine's
 * own queue instead of the harness default. Return a transport vtable, or do
 * not export the symbol at all to accept the default. The harness calls
 * create() on the returned vtable and passes the handle back through
 * engine_init(). An engine whose matcher already emits into its own queue may
 * implement only create()/drain()/flush()/destroy() and leave push() unused.
 * ===========================================================================*/

const me_transport_t* engine_get_transport(void);

/* ===========================================================================
 * OPTIONAL: pre-build hook
 *
 * If exported, the harness calls engine_prebuild() once per workload message,
 * in dispatch order, BEFORE the timed window opens — letting the engine marshal
 * each message into its native order representation ahead of time. The timed
 * engine_on_* calls then dispatch the pre-built form, so the measured window is
 * matching work alone, not ABI-struct-to-native marshaling. msg_type is
 * 0 = new, 1 = cancel, 2 = modify; msg points to the matching new_order_t /
 * cancel_t / modify_t. An engine that does not export this converts inside
 * engine_on_* as usual.
 * ===========================================================================*/

void engine_prebuild(uint8_t msg_type, const void* msg);

#ifdef __cplusplus
}  /* extern "C" */

static_assert(sizeof(new_order_t) == 32, "new_order_t must be 32 bytes");
static_assert(sizeof(cancel_t)    == 16, "cancel_t must be 16 bytes");
static_assert(sizeof(modify_t)    == 32, "modify_t must be 32 bytes");
static_assert(sizeof(me_report_t) == 64, "me_report_t must be 64 bytes");
#endif

#endif /* MATCHING_ENGINE_API_H */
