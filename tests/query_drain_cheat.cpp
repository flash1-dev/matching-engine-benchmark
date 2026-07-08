/*
 * query_drain_cheat.cpp — PoC "lazy matching" adapter for testing FIX ① (the
 * harness's probe-time-exclusion bound in src/harness.cpp). This is NOT a
 * baseline; it is the exact attack the PROBE_FAIL_FRAC gate exists to catch.
 *
 * It is byte-for-byte the SAME Liquibook matching as adapters/liquibook_adapter.cpp
 * (genuine order book, identical report stream) — the only change is WHERE the
 * matching runs. engine_on_new_order / engine_on_cancel / engine_on_modify no
 * longer call into Liquibook at all: they just append the raw message to a
 * FIFO queue (g_pending) and return — O(1), no matching. The real matching
 * (do_new_order / do_cancel / do_modify below, copied verbatim from
 * liquibook_adapter.cpp, only renamed) now runs inside drain(), which is
 * called from engine_query_best_bid, engine_query_best_ask,
 * engine_query_depth_at, AND engine_flush — i.e. the book only advances when
 * the harness probes it or flushes.
 *
 * Why this is a cheat: the harness times engine_on_* (the dispatch loop) but
 * EXCLUDES engine_query_* time from throughput (excluded_ns), on the theory
 * that an honest engine shouldn't be charged for the harness's own book
 * inspections. Only AUDIT_POINTS=64 probes happen across the whole run
 * (src/harness.cpp), so whatever has piled up in g_pending since the last
 * probe/flush gets matched inside the NEXT probe call (or, for the final
 * slice, inside engine_flush() at the very end). That pushes essentially all
 * the real matching work into either the excluded window (~63/64 of it,
 * amortized inside probe calls) or the small tail processed by engine_flush()
 * — while engine_on_* itself does only a cheap buffer append. The reported
 * throughput (workload.size() / elapsed) is therefore implausibly high, and
 * excluded_ns dominates the raw wall-clock window.
 *
 * This is exactly the attack src/harness.cpp's excluded_frac gate exists to
 * catch (PROBE_FAIL_FRAC = 0.25): an honest engine's probes are a negligible
 * slice of the run; here they are nearly all of it. Expected verdict: INVALID
 * in BOTH --mode perf and --mode audit, printing
 *   "Anti-cheat: audit probes consumed NN.N% of the timed window — INVALID:
 *    matcher work hidden in engine_query_*"
 *
 * The book state itself stays fully CORRECT throughout — drain() always
 * catches the book up through the message just dispatched before any query
 * answers, so the harness's audit-mode state-audit comparison against the
 * liquibook baseline still PASSes, and the report-stream hash still matches
 * the published reference. That is the point: this proves the probe-time gate
 * is an INDEPENDENT check, not a proxy for correctness — a genuinely correct,
 * byte-identical engine still gets caught and gated INVALID for hiding matcher
 * work off the clock.
 *
 * Build (same link recipe as scripts/build_baselines.sh's build_liquibook,
 * against the same pinned third_party/liquibook checkout):
 *   g++ -std=c++20 -O3 -march=native -fPIC -shared -fexceptions -frtti \
 *       -I api -I third_party/liquibook/src \
 *       tests/query_drain_cheat.cpp third_party/liquibook/src/simple/simple_order.cpp \
 *       -o query_drain_cheat.so
 *
 * Run:
 *   ./harness --engine ./query_drain_cheat.so --scenario normal --seed 23 \
 *             --count 1000000 --mode perf
 *       -> implausibly high throughput; Anti-cheat probe-time line; INVALID
 *   ./harness --engine ./query_drain_cheat.so --scenario normal --seed 23 \
 *             --count 1000000 --mode audit
 *       -> State audit: PASS (book is genuinely correct) but still INVALID
 *          (probe-time gate fires independently of the state audit)
 */
#define LIQUIBOOK_ORDER_KNOWS_CONDITIONS
#include <simple/simple_order.h>
#include <simple/simple_order_book.h>

#include "matching_engine_api.h"

#include <cstdint>
#include <vector>

namespace {

using LO    = liquibook::simple::SimpleOrder;
using LBook = liquibook::simple::SimpleOrderBook<5>;
namespace lb = liquibook::book;

LBook* g_book = nullptr;
std::vector<LO*>      g_orders;     // harness order_id -> current LO
std::vector<uint64_t> g_lb2ext;     // liquibook order_id_ -> harness id
std::vector<LO*>      g_all;        // every LO created, for cleanup

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

uint64_t g_seq    = 0;     // aggressive order's sequence number (current call)
uint64_t g_filled = 0;     // quantity the aggressive order filled this call

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin */ }
}

void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
              uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    push_report(r);
}

/* Liquibook reports fills through this listener during add() — identical to
 * liquibook_adapter.cpp's Listener. */
class Listener : public lb::OrderListener<LO*> {
public:
    void on_accept(LO* const&) override {}
    void on_fill(LO* const& taker, LO* const& maker,
                 lb::Quantity qty, lb::Price price) override {
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = g_seq;
        r.price_ticks     = static_cast<int64_t>(price);
        r.quantity        = static_cast<uint32_t>(qty);
        r.maker_order_id  = g_lb2ext[maker->order_id_];
        r.taker_order_id  = g_lb2ext[taker->order_id_];
        push_report(r);
        g_filled += qty;
    }
    void on_cancel(LO* const&) override {}
    void on_replace(LO* const&, const int64_t&, lb::Price) override {}
    void on_reject(LO* const&, const char*) override {}
    void on_cancel_reject(LO* const&, const char*) override {}
    void on_replace_reject(LO* const&, const char*) override {}
};
Listener g_listener;

inline bool resting(LO* lo) {
    return lo && lo->state() == liquibook::simple::os_accepted
              && lo->open_qty() > 0;
}

inline LO* find_order(uint64_t ext_id) {
    return ext_id < g_orders.size() ? g_orders[ext_id] : nullptr;
}

LO* make_order(bool is_buy, int64_t price, uint32_t qty, lb::OrderConditions cond) {
    LO* lo = new LO(is_buy, static_cast<lb::Price>(price),
                    static_cast<lb::Quantity>(qty), 0, cond);
    g_all.push_back(lo);
    return lo;
}

inline void map_lb_id(LO* lo, uint64_t ext_id) {
    g_lb2ext[lo->order_id_] = ext_id;
}

/* ---- the REAL matching logic: byte-for-byte liquibook_adapter.cpp's
 * engine_on_new_order / engine_on_cancel / engine_on_modify, only renamed so
 * drain() (below) calls them lazily instead of the harness calling them
 * directly on the clock. Nothing about the matching semantics changes. ---- */

void do_new_order(const new_order_t* o) {
    g_seq    = o->sequence_number;
    g_filled = 0;
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    lb::OrderConditions cond = o->ioc ? lb::oc_immediate_or_cancel
                                      : lb::oc_no_conditions;
    LO* lo = make_order(o->side == 0, o->price_ticks, o->quantity, cond);
    map_lb_id(lo, o->order_id);
    g_orders[o->order_id] = lo;
    g_book->add(lo, cond);   // fills delivered as Trade reports via Listener::on_fill

    if (o->ioc && g_filled < o->quantity)            // IOC residual cancellation
        emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                 o->price_ticks, static_cast<uint32_t>(o->quantity - g_filled));
}

void do_cancel(const cancel_t* c) {
    LO* lo = find_order(c->order_id);
    if (resting(lo)) {
        g_book->cancel(lo);
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 lo->is_buy() ? 0 : 1, static_cast<int64_t>(lo->price()), 0);
    } else {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void do_modify(const modify_t* m) {
    g_seq    = m->sequence_number;
    g_filled = 0;
    LO* cur = find_order(m->order_id);
    if (resting(cur)) {
        g_book->cancel(cur);
        LO* lo = make_order(m->side == 0, m->new_price_ticks,
                            m->new_quantity, lb::oc_no_conditions);
        map_lb_id(lo, m->order_id);
        g_orders[m->order_id] = lo;   // overwrite: same slot
        g_book->add(lo, lb::oc_no_conditions);   // crossing fills -> Trade reports
        emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
                 m->side, m->new_price_ticks, m->new_quantity);
    } else {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
    }
}

/* ---- THE CHEAT: the incoming-message queue + the lazy drain ---- */

std::vector<me_msg_t> g_pending;      // buffered, not-yet-matched messages
size_t                g_drain_pos = 0;

void drain() {
    while (g_drain_pos < g_pending.size()) {
        const me_msg_t& m = g_pending[g_drain_pos++];
        if (m.type == 0)      do_new_order(&m.no);
        else if (m.type == 1) do_cancel(&m.c);
        else                  do_modify(&m.md);
    }
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_book = new LBook();
    g_book->set_order_listener(&g_listener);
    g_orders.resize(1u << 21, nullptr);
    g_lb2ext.resize(1u << 21, 0);
    g_all.reserve(1u << 21);
    g_pending.reserve(4u << 20);       // headroom well above any test count
    g_drain_pos = 0;
}

void engine_shutdown(void) {
    for (LO* lo : g_all) delete lo;
    g_all.clear();
    g_orders.clear();
    g_lb2ext.clear();
    g_pending.clear();
    g_drain_pos = 0;
    delete g_book;
    g_book = nullptr;
}

/* THE CHEAT, part 1: buffer only — no matching happens on this call at all. */
void engine_on_new_order(const new_order_t* o) {
    me_msg_t m{}; m.type = 0; m.no = *o; g_pending.push_back(m);
}
void engine_on_cancel(const cancel_t* c) {
    me_msg_t m{}; m.type = 1; m.c = *c; g_pending.push_back(m);
}
void engine_on_modify(const modify_t* md) {
    me_msg_t m{}; m.type = 2; m.md = *md; g_pending.push_back(m);
}

/* THE CHEAT, part 2: the deferred matching work happens here — inside the
 * pipeline barrier the harness DOES time (but only pays for the tail slice
 * since the last probe), and inside the three query calls, whose wall time the
 * harness EXCLUDES from throughput. */
void engine_flush(void) { drain(); }

int64_t engine_query_best_bid(void) {
    drain();
    const auto& b = g_book->bids();
    return b.empty() ? INT64_MIN
                     : static_cast<int64_t>(b.begin()->first.price());
}

int64_t engine_query_best_ask(void) {
    drain();
    const auto& a = g_book->asks();
    return a.empty() ? INT64_MAX
                     : static_cast<int64_t>(a.begin()->first.price());
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    drain();
    const lb::Price p = static_cast<lb::Price>(price_ticks);
    uint64_t total = 0;
    if (side == 0) {
        auto range = g_book->bids().equal_range(lb::ComparablePrice(true, p));
        for (auto it = range.first; it != range.second; ++it)
            total += it->second.open_qty();
    } else {
        auto range = g_book->asks().equal_range(lb::ComparablePrice(false, p));
        for (auto it = range.first; it != range.second; ++it)
            total += it->second.open_qty();
    }
    return total;
}

}  // extern "C"
