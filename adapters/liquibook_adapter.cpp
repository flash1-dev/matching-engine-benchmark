/*
 * liquibook_adapter.cpp — matching_engine_api.h backed by Liquibook.
 *
 * Liquibook: https://github.com/ObjectComputing/liquibook  (tree-of-lists book).
 * Built as a shared library by scripts/build_baselines.sh. Every deviation from
 * Liquibook's upstream behaviour is documented in docs/PATCHES.md.
 *
 * The adapter emits the report stream itself: an OrderAck per new order, a
 * Trade per fill (via Liquibook's OrderListener callback), a CancelAck per
 * cancel and per IOC residual, and a ModifyAck per modify — each pushed into
 * the harness report transport. Liquibook matches synchronously on the calling
 * thread, so engine_flush() is a no-op. Modify is handled as cancel + reinsert
 * (Liquibook's replace() corrupts SimpleOrder::price_; see docs/PATCHES.md).
 */
#define LIQUIBOOK_ORDER_KNOWS_CONDITIONS
#include <simple/simple_order.h>
#include <simple/simple_order_book.h>

#include "matching_engine_api.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

namespace {

using LO    = liquibook::simple::SimpleOrder;
using LBook = liquibook::simple::SimpleOrderBook<5>;
namespace lb = liquibook::book;

LBook* g_book = nullptr;
std::unordered_map<uint64_t, LO*>      g_orders;   // harness order_id -> current LO
std::unordered_map<uint32_t, uint64_t> g_lb2ext;   // liquibook id    -> harness id
std::vector<LO*>                       g_all;      // every LO created, for cleanup
std::vector<LO*>                       g_pre;      // pre-built new-order LOs (prebuild)
size_t                                 g_pre_idx = 0;

const me_transport_t* g_transport = nullptr;       // harness report transport
void*                 g_sink      = nullptr;

uint64_t g_seq    = 0;     // aggressive order's sequence number (current call)
uint64_t g_filled = 0;     // quantity the aggressive order filled this call

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
}

/* Emit a non-trade report (OrderAck / CancelAck / ModifyAck). */
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

/* Liquibook reports fills through this listener during add(). */
class Listener : public lb::OrderListener<LO*> {
public:
    void on_accept(LO* const&) override {}
    void on_fill(LO* const& taker, LO* const& maker,
                 lb::Quantity qty, lb::Price price) override {
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = g_seq;
        r.price_ticks     = static_cast<int64_t>(price);   // maker's resting price
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

LO* make_order(bool is_buy, int64_t price, uint32_t qty, lb::OrderConditions cond) {
    LO* lo = new LO(is_buy, static_cast<lb::Price>(price),
                    static_cast<lb::Quantity>(qty), 0, cond);
    g_all.push_back(lo);
    return lo;
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_book = new LBook();
    g_book->set_order_listener(&g_listener);
    g_orders.reserve(1u << 21);
    g_lb2ext.reserve(1u << 21);
    g_all.reserve(1u << 21);
}

void engine_shutdown(void) {
    for (LO* lo : g_all) delete lo;
    g_all.clear();
    g_orders.clear();
    g_lb2ext.clear();
    delete g_book;
    g_book = nullptr;
}

/* Liquibook matches synchronously on the calling thread — nothing is pending. */
void engine_flush(void) {}

/* Pre-build hook: construct each new order's native SimpleOrder before the timed
 * window, so engine_on_new_order's measured work is the match alone. */
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type != 0) return;
    const new_order_t* o = static_cast<const new_order_t*>(msg);
    lb::OrderConditions cond = o->ioc ? lb::oc_immediate_or_cancel
                                      : lb::oc_no_conditions;
    g_pre.push_back(make_order(o->side == 0, o->price_ticks, o->quantity, cond));
}

void engine_on_new_order(const new_order_t* o) {
    g_seq    = o->sequence_number;
    g_filled = 0;
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    lb::OrderConditions cond = o->ioc ? lb::oc_immediate_or_cancel
                                      : lb::oc_no_conditions;
    LO* lo = g_pre[g_pre_idx++];   // native SimpleOrder built by engine_prebuild
    g_orders[o->order_id]   = lo;
    g_lb2ext[lo->order_id_] = o->order_id;
    g_book->add(lo, cond);   // fills delivered as Trade reports via Listener::on_fill

    if (o->ioc && g_filled < o->quantity)            // IOC residual cancellation
        emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                 o->price_ticks, static_cast<uint32_t>(o->quantity - g_filled));
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_orders.find(c->order_id);
    if (it != g_orders.end() && resting(it->second)) {
        LO* lo = it->second;
        g_book->cancel(lo);
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 lo->is_buy() ? 0 : 1, static_cast<int64_t>(lo->price()), 0);
    } else {
        // Order is not resting — already filled, already cancelled, or never
        // seen (a duplicate/stale cancel). Answer with a reject, not an ack.
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    g_seq    = m->sequence_number;
    g_filled = 0;
    auto it = g_orders.find(m->order_id);
    if (it != g_orders.end() && resting(it->second)) {
        /* Cancel + reinsert at the new price/quantity (loses queue priority —
         * the production rule for a reprice or a quantity increase). */
        g_book->cancel(it->second);
        LO* lo = make_order(m->side == 0, m->new_price_ticks, m->new_quantity,
                            lb::oc_no_conditions);
        g_orders[m->order_id]   = lo;
        g_lb2ext[lo->order_id_] = m->order_id;
        g_book->add(lo, lb::oc_no_conditions);   // crossing fills -> Trade reports
        emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
                 m->side, m->new_price_ticks, m->new_quantity);
    } else {
        // Order not resting — a duplicate/stale modify. Answer with a reject.
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
    }
}

int64_t engine_query_best_bid(void) {
    const auto& b = g_book->bids();
    return b.empty() ? INT64_MIN
                     : static_cast<int64_t>(b.begin()->first.price());
}

int64_t engine_query_best_ask(void) {
    const auto& a = g_book->asks();
    return a.empty() ? INT64_MAX
                     : static_cast<int64_t>(a.begin()->first.price());
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const lb::Price p = static_cast<lb::Price>(price_ticks);
    uint64_t total = 0;
    if (side == 0) {
        for (const auto& kv : g_book->bids())
            if (kv.first.price() == p) total += kv.second.open_qty();
    } else {
        for (const auto& kv : g_book->asks())
            if (kv.first.price() == p) total += kv.second.open_qty();
    }
    return total;
}

}  // extern "C"
