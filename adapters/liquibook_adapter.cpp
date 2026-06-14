/*
 * liquibook_adapter.cpp — matching_engine_api.h backed by Liquibook.
 *
 * Liquibook: https://github.com/ObjectComputing/liquibook  (price-keyed
 * multimap book). Built as a shared library by scripts/build_baselines.sh.
 * Every deviation from Liquibook's upstream behaviour is documented in
 * docs/PATCHES.md.
 *
 * The adapter emits the report stream itself: an OrderAck per new order, a
 * Trade per fill (via Liquibook's OrderListener callback), a CancelAck per
 * cancel and per IOC residual, a ModifyAck per modify, and a CancelReject /
 * ModifyReject per cancel/modify of an order that is not resting — each
 * pushed into the harness report transport. Liquibook matches synchronously
 * on the calling thread, so engine_flush() is a no-op. Modify is handled as
 * cancel + reinsert because that is the harness contract
 * (api/matching_engine_api.h: every modify loses time priority); Liquibook's
 * native replace() updates same-price quantity changes in place — keeping
 * priority — so it implements different semantics and is not used.
 */
// Documentary only: simple_order.h defines LIQUIBOOK_ORDER_KNOWS_CONDITIONS
// itself before any consumer sees it, and the code it gates is the inert
// constructor OR documented in docs/PATCHES.md. The IOC flag actually
// travels through the explicit add() conditions argument below.
#define LIQUIBOOK_ORDER_KNOWS_CONDITIONS
#include <simple/simple_order.h>
#include <simple/simple_order_book.h>

#include "matching_engine_api.h"

#include <algorithm>
#include <cstdint>
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
/* Harness order ids are dense and 1-based (a permutation of 1..N_new), so a
 * flat vector indexed by order_id replaces a hash map: lookup is a bounds
 * check + load (nullptr = never seen / vector slot never written). The slots
 * are SIZED in engine_init / engine_prebuild (capacity only, untimed — the
 * static-allocation parity every fixed-array engine gets), but every per-order
 * WRITE happens on the clock in engine_on_*. The id->LO* mapping is NECESSARY:
 * Liquibook cancels by order POINTER and keeps no id index of its own. */
std::vector<LO*>      g_orders;     // harness order_id -> current LO
/* Reverse map for trade-report labeling. Liquibook assigns order_id_ =
 * ++last_order_id_ when the SimpleOrder is CONSTRUCTED; construction now
 * happens on the clock (engine_on_*), so this mapping is written there too —
 * engine_prebuild only pre-sizes the table's capacity (g_lb_built). */
std::vector<uint64_t> g_lb2ext;     // liquibook order_id_ -> harness id
std::vector<LO*>      g_all;        // every LO created, for cleanup
size_t                g_lb_built = 0;   // SimpleOrders the timed loop will build
                                        // (news + modifies, an upper bound);
                                        // pre-sizes g_lb2ext in engine_prebuild

const me_transport_t* g_transport = nullptr;       // harness report transport
void*                 g_sink      = nullptr;

uint64_t g_seq    = 0;     // aggressive order's sequence number (current call)
uint64_t g_filled = 0;     // quantity the aggressive order filled this call

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
}

/* Emit a non-trade report (OrderAck / CancelAck / ModifyAck / CancelReject /
 * ModifyReject). */
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
        // Indexed loads into the reverse map, written on the clock when each
        // SimpleOrder was constructed (engine_on_*); only its capacity is
        // pre-sized.
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

/* Bounds-checked flat-vector lookup: nullptr = not seen (slot never written
 * or id past the populated range) — the same outcomes a map miss produced. */
inline LO* find_order(uint64_t ext_id) {
    return ext_id < g_orders.size() ? g_orders[ext_id] : nullptr;
}

LO* make_order(bool is_buy, int64_t price, uint32_t qty, lb::OrderConditions cond) {
    LO* lo = new LO(is_buy, static_cast<lb::Price>(price),
                    static_cast<lb::Quantity>(qty), 0, cond);
    g_all.push_back(lo);
    return lo;
}

/* Record lo's liquibook-id -> harness-id pair. Called on the clock as each
 * order is built; the slot was pre-sized in engine_prebuild (g_lb_built bounds
 * order_id_), so this is a plain indexed store with no capacity check. */
inline void map_lb_id(LO* lo, uint64_t ext_id) {
    g_lb2ext[lo->order_id_] = ext_id;
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_book = new LBook();
    g_book->set_order_listener(&g_listener);
    /* resize (not reserve): zero-fills the slots and faults the pages in,
     * both outside the timed window. engine_prebuild grows past this if a
     * workload ever uses ids beyond 2M. */
    g_orders.resize(1u << 21, nullptr);
    g_lb2ext.resize(1u << 21, 0);
    g_all.reserve(1u << 21);
}

void engine_shutdown(void) {
    for (LO* lo : g_all) delete lo;
    g_all.clear();
    g_orders.clear();
    g_lb2ext.clear();
    g_lb_built = 0;
    delete g_book;
    g_book = nullptr;
}

/* Liquibook matches synchronously on the calling thread — nothing is pending. */
void engine_flush(void) {}

/* Pre-build hook: capacity pre-sizing ONLY (untimed) — the static-allocation
 * parity a fixed-capacity engine gets at init. NOTHING per-order is built here:
 * the SimpleOrder, both its id mappings, and the book insert all happen on the
 * clock in engine_on_*, as the prebuild contract requires (prebuild may only
 * translate + pre-size capacity; node alloc + id registration stay timed). One SimpleOrder is
 * constructed per new and per modify (Liquibook sets order_id_ = ++last_order_id_
 * at construction), so g_lb_built upper-bounds order_id_ and pre-sizes the
 * reverse table; a cancel constructs nothing. */
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type == 1) return;                         // a cancel builds no order
    if (msg_type == 0) {
        const new_order_t* o = static_cast<const new_order_t*>(msg);
        if (o->order_id >= g_orders.size())
            g_orders.resize(std::max<size_t>(g_orders.size() * 2,
                                             static_cast<size_t>(o->order_id) + 1),
                            nullptr);
    }
    if (++g_lb_built >= g_lb2ext.size())
        g_lb2ext.resize(std::max<size_t>(g_lb2ext.size() * 2, g_lb_built + 1), 0);
}

void engine_on_new_order(const new_order_t* o) {
    g_seq    = o->sequence_number;
    g_filled = 0;
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    lb::OrderConditions cond = o->ioc ? lb::oc_immediate_or_cancel
                                      : lb::oc_no_conditions;
    // Native-object construction (alloc + Liquibook id) and BOTH id-map writes
    // are on the clock — the gateway only translated the wire message; the
    // matcher builds, indexes, and inserts the order.
    LO* lo = make_order(o->side == 0, o->price_ticks, o->quantity, cond);
    map_lb_id(lo, o->order_id);    // reverse map (for fill labeling)
    g_orders[o->order_id] = lo;    // forward map; slot pre-sized in prebuild
    g_book->add(lo, cond);   // fills delivered as Trade reports via Listener::on_fill

    if (o->ioc && g_filled < o->quantity)            // IOC residual cancellation
        emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                 o->price_ticks, static_cast<uint32_t>(o->quantity - g_filled));
}

void engine_on_cancel(const cancel_t* c) {
    LO* lo = find_order(c->order_id);
    if (resting(lo)) {
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
    LO* cur = find_order(m->order_id);
    if (resting(cur)) {
        /* Cancel + reinsert at the new price/quantity (loses queue priority —
         * the production rule for a reprice or a quantity increase). The
         * reinsert order is built and indexed here, on the clock. */
        g_book->cancel(cur);
        LO* lo = make_order(m->side == 0, m->new_price_ticks,
                            m->new_quantity, lb::oc_no_conditions);
        map_lb_id(lo, m->order_id);
        g_orders[m->order_id] = lo;   // overwrite: same slot
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
    /* Keyed multimap access (equal_range on ComparablePrice) instead of a
     * full-side walk — the book's own index answers depth-at-price. */
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
