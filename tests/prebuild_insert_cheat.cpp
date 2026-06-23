/*
 * prebuild_insert_cheat.cpp — proves the harness's pre-flight book-empty assert
 * is load-bearing. This is NOT a matching engine and is not a baseline.
 *
 * It does its book INSERT during engine_prebuild — before the clock — and
 * leaves the timed engine_on_new_order a bare OrderAck. By moving the resting
 * work ahead of the timed window it posts an inflated throughput, exactly the
 * hoist the api contract forbids (api/matching_engine_api.h: engine_prebuild is
 * translation-only). The harness queries the book between the prebuild pass and
 * t0; because this engine rested orders during prebuild, the book is non-empty
 * there and the run is gated INVALID with an "Anti-cheat: pre-start book not
 * empty by the API sentinels" line — regardless of whether the output hash matches.
 *
 * Contrast with tests/cheat_adapter.cpp, which keeps an empty book before t0
 * (so it passes this pre-flight) and is instead caught by the mode=audit state
 * audit. Two cheats, two layers.
 *
 * Build and run:
 *   g++ -std=c++20 -O3 -fPIC -shared -I api tests/prebuild_insert_cheat.cpp \
 *       -o prebuild_insert_cheat.so
 *   ./harness --engine ./prebuild_insert_cheat.so --scenario normal --mode perf
 *       -> Verdict: INVALID  (pre-flight: book non-empty before the timed window)
 */
#include "matching_engine_api.h"

#include <cstdint>
#include <map>

namespace {
const me_transport_t* g_t = nullptr;
void*                 g_sink = nullptr;
std::map<int64_t, uint64_t> g_bids, g_asks;   // price -> qty (a real book)

void emit(uint8_t type, uint64_t seq, uint64_t id, uint8_t side,
          int64_t px, uint32_t q) {
    me_report_t r{};
    r.type = type; r.sequence_number = seq; r.order_id = id;
    r.side = side; r.price_ticks = px; r.quantity = q;
    while (!g_t->push(g_sink, &r)) { /* spin */ }
}
}  // namespace

extern "C" {

void engine_init(uint64_t, const me_transport_t* t, void* s) { g_t = t; g_sink = s; }
void engine_shutdown(void) { g_bids.clear(); g_asks.clear(); }
void engine_flush(void) {}

/* THE CHEAT: rest each order into the book here, ahead of the timed window. */
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type != 0) return;
    auto* o = static_cast<const new_order_t*>(msg);
    (o->side == 0 ? g_bids : g_asks)[o->price_ticks] += o->quantity;
}

/* Timed path: a bare ack — the resting work already happened in prebuild. */
void engine_on_new_order(const new_order_t* o) {
    emit(ME_ORDER_ACK, o->sequence_number, o->order_id, o->side,
         o->price_ticks, o->quantity);
}
void engine_on_cancel(const cancel_t* c) {
    emit(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
}
void engine_on_modify(const modify_t* m) {
    emit(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
}

int64_t  engine_query_best_bid(void) { return g_bids.empty() ? INT64_MIN : g_bids.rbegin()->first; }
int64_t  engine_query_best_ask(void) { return g_asks.empty() ? INT64_MAX : g_asks.begin()->first; }
uint64_t engine_query_depth_at(int64_t px, uint8_t side) {
    auto& m = (side == 0) ? g_bids : g_asks;
    auto it = m.find(px);
    return it == m.end() ? 0 : it->second;
}

}  // extern "C"
