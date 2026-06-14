/*
 * shadow_prematch_cheat.cpp — proves the harness's prebuild-TIME bound is
 * load-bearing. This is NOT a matching engine and is not a baseline.
 *
 * It front-loads matcher-class work into engine_prebuild but keeps its book
 * PRIVATE, so the pre-flight book-empty assert (which queries the book) sees an
 * empty book and lets it through. The timed engine_on_* calls are bare acks.
 * Because the matching cost moved off the clock, the prebuild pass runs many
 * times longer than the timed window, and the harness's prebuild-time bound
 * (T_prebuild / T_timed) flags it past 2x and gates it INVALID past 4x -- a
 * level no honest translation reaches. This is the cheat the book-empty assert
 * cannot see; the time bound is its complement.
 *
 * The per-message work here is a stand-in for a real pre-match (a price-keyed
 * book update plus match-class compute). A real cheat reaches a high ratio a
 * different way: to actually post a fast number it needs a small timed window
 * (an efficient report path), which is itself what makes T_prebuild / T_timed
 * large. Either way the bound fires. (It does not reproduce the hash; the bound
 * gates regardless.)
 *
 * Build and run:
 *   g++ -std=c++20 -O3 -fPIC -shared -I api tests/shadow_prematch_cheat.cpp \
 *       -o shadow_prematch_cheat.so
 *   ./harness --engine ./shadow_prematch_cheat.so --scenario normal --mode perf
 *       -> Verdict: INVALID  ("Anti-cheat: pre-build ran Nx the timed window")
 */
#include "matching_engine_api.h"

#include <cstdint>
#include <map>

namespace {
const me_transport_t* g_t = nullptr; void* g_sink = nullptr;
std::map<int64_t, uint64_t> g_b, g_a;   // PRIVATE shadow book — never exposed
uint64_t g_busy = 0;

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
void engine_shutdown(void) { g_b.clear(); g_a.clear(); }
void engine_flush(void) {}

/* THE CHEAT: do the matching work here, into the private shadow. */
void engine_prebuild(uint8_t mt, const void* msg) {
    if (mt != 0) return;
    auto* o = static_cast<const new_order_t*>(msg);
    (o->side == 0 ? g_b : g_a)[o->price_ticks] += o->quantity;   // book update
    volatile uint64_t s = g_busy;                                // match-class compute
    for (int k = 0; k < 512; ++k) s = s * 1664525u + 1013904223u + o->order_id;
    g_busy = s;
}

/* Timed path: bare acks — the work already happened in prebuild. */
void engine_on_new_order(const new_order_t* o) {
    emit(ME_ORDER_ACK, o->sequence_number, o->order_id, o->side, o->price_ticks, o->quantity);
}
void engine_on_cancel(const cancel_t* c) { emit(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0); }
void engine_on_modify(const modify_t* m) { emit(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0); }

/* Claim empty — the shadow is private, so the book-empty pre-flight passes. */
int64_t  engine_query_best_bid(void) { return INT64_MIN; }
int64_t  engine_query_best_ask(void) { return INT64_MAX; }
uint64_t engine_query_depth_at(int64_t, uint8_t) { return 0; }

}  // extern "C"
