/*
 * transport.cpp — the harness's inter-thread report transports.
 *
 * The harness models a production exchange's matcher -> outbound-publisher
 * hand-off: the engine pushes its report stream (OrderAck / Trade / CancelAck /
 * ModifyAck) into a queue, and a drainer thread on an adjacent core consumes
 * it. The push cost is inside the measured window.
 *
 * Two transports live here:
 *   harness_default_transport() — a boost::lockfree single-producer /
 *     single-consumer queue, used unless the engine supplies its own via
 *     engine_get_transport() (see api/matching_engine_api.h).
 *   harness_null_transport()    — a discard sink used for the audit's baseline
 *     replay, whose report stream is not needed.
 */
#include "harness.h"
#include <boost/lockfree/spsc_queue.hpp>

namespace {

using ReportQueue = boost::lockfree::spsc_queue<me_report_t>;

void* dflt_create(uint32_t capacity) {
    return new (std::nothrow) ReportQueue(capacity);
}

int dflt_push(void* handle, const me_report_t* report) {
    return static_cast<ReportQueue*>(handle)->push(*report) ? 1 : 0;
}

uint32_t dflt_drain(void* handle, me_report_t* out, uint32_t max) {
    return static_cast<uint32_t>(
        static_cast<ReportQueue*>(handle)->pop(out, max));
}

void dflt_flush(void* /*handle*/) {
    /* boost::lockfree::spsc_queue publishes on every push — nothing to flush. */
}

void dflt_destroy(void* handle) {
    delete static_cast<ReportQueue*>(handle);
}

const me_transport_t DEFAULT_TRANSPORT = {
    dflt_create, dflt_push, dflt_drain, dflt_flush, dflt_destroy
};

/* ---- null transport: accept and discard ---------------------------------
 * The audit replays the workload through a baseline engine purely to read its
 * order-book state via engine_query_*; the reports that baseline emits are not
 * needed, so they are dropped rather than queued and drained. create() returns
 * a non-NULL sentinel that nothing dereferences. */
void* null_create(uint32_t /*capacity*/)                            { return reinterpret_cast<void*>(1); }
int null_push(void* /*handle*/, const me_report_t* /*report*/)      { return 1; }
uint32_t null_drain(void* /*handle*/, me_report_t* /*out*/, uint32_t /*max*/) { return 0; }
void null_flush(void* /*handle*/)                                   {}
void null_destroy(void* /*handle*/)                                 {}

const me_transport_t NULL_TRANSPORT = {
    null_create, null_push, null_drain, null_flush, null_destroy
};

}  // namespace

const me_transport_t* harness_default_transport() { return &DEFAULT_TRANSPORT; }
const me_transport_t* harness_null_transport()    { return &NULL_TRANSPORT; }
