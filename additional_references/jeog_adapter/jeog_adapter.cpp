/*
 * jeog_adapter.cpp — matching_engine_api.h backed by jeog/SimpleOrderbook.
 *
 * SimpleOrderbook (Jonathon Ogden, https://github.com/jeog/SimpleOrderbook):
 * a price-time-priority limit order book whose price levels are a flat,
 * directly-indexed std::vector<level> spanning a fixed [min,max] price range
 * (one `level` slot per tick). Orders enter through an external order queue
 * serviced by a dispatcher thread; the public *synchronous* entry points
 * (insert_limit_order / pull_order / replace_with_limit_order) BLOCK on a
 * future until the dispatcher has fully processed the order under the engine's
 * master mutex, and then run that order's synchronous execution callbacks on
 * the CALLING thread before returning. So from the adapter's view every call
 * is synchronous and ordered, and engine_flush() is a no-op.
 *
 * Report mapping
 * --------------
 *  - OrderAck   : emitted by the adapter, one per accepted new order.
 *  - Trade      : the engine's per-fill execution callback. The engine fires
 *                 the callback ONCE PER SIDE per fill, each as
 *                 (callback_msg::fill, own_id, own_id, maker_price, qty) — the
 *                 buy side's notification then the sell side's, back to back,
 *                 for the same trade (core.cpp _trade_has_occured). The taker
 *                 is the order currently being inserted (its engine id is known
 *                 to the adapter); of each pair the OTHER notification is the
 *                 maker. We emit exactly one Trade on the maker-side callback
 *                 (price = the maker's resting price, which is what the engine
 *                 passes), so fills come out in match order with correct
 *                 maker/taker ids and no double counting.
 *  - CancelAck  : one per successful pull_order, and one per IOC residual.
 *  - ModifyAck  : one per successful modify (cancel + reinsert).
 *  - Cancel/ModifyReject : pull_order(id) returns false for an order that is
 *                 not resting (never seen / already filled / already pulled),
 *                 which is the engine's own native liveness answer.
 *
 * IOC: the engine has no native partial-IOC for limit orders (its advanced FOK
 * is all-or-none, different semantics). We map IOC faithfully as: insert the
 * limit, let it match, then if any quantity remains resting, pull it and emit
 * the residual CancelAck. The match-what-you-can-drop-the-rest result is
 * identical to a native IOC.
 *
 * Modify is cancel + reinsert (the harness contract): pull the old order, then
 * insert a fresh limit at the new price/quantity (new engine id, losing queue
 * priority) and remap the harness id to it. The engine's own replace_with_*
 * does the same pull+reinsert internally, but doing it in the adapter lets us
 * detect "not resting" (ModifyReject) and control the id remap.
 *
 * Price mapping: the book uses ratio<1,1> (tick size 1.0) so engine price ==
 * harness integer tick directly. The book is created spanning a wide positive
 * tick range that covers every canonical/held-out workload (mid 33504 ticks,
 * widest observed swing ~[8900, 81700]); if an out-of-range price ever
 * arrives the book is grown to fit (engine ManagementInterface, untimed-rare).
 *
 * No upstream engine source is patched. The library is compiled exactly as
 * shipped (its makefile builds at -std=c++11); see build.sh.
 */
#include "simpleorderbook.hpp"

#include "matching_engine_api.h"

#include <cstdint>
#include <cmath>
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

using sob::FullInterface;
using sob::ManagementInterface;
using sob::id_type;
using sob::callback_msg;

// ratio<1,1> => tick size 1.0 => engine price (a double) == harness tick.
using WholeTick = std::ratio<1,1>;

// Book price range, in ticks. Mid is ~33504 ticks (167.52/0.005); the widest
// price excursion seen across every scenario/seed is roughly [8900, 81700].
// [1, 2^18-1] gives a large margin; out-of-range prices grow the book.
constexpr double BOOK_MIN = 1.0;
constexpr double BOOK_MAX = 262143.0;     // 2^18 - 1

FullInterface*       g_book = nullptr;
ManagementInterface* g_mgmt = nullptr;

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// harness order_id -> current engine id (0 = none). Dense ids (1..N), so a flat
// vector replaces a hash map. The reverse map labels fills (engine id ->
// harness id). Engine ids are dense (++ per limit insert; we never use stops),
// so a flat vector works there too. Both sized in engine_init/prebuild
// (capacity only, untimed); every per-order WRITE is on the clock.
std::vector<id_type>  g_h2e;          // harness id -> engine id
std::vector<uint64_t> g_e2h;          // engine id  -> harness id
// Minimal liveness/identity shadow: the harness cancel_t carries no side/price,
// and the engine infers a resting limit's side from its level vs the ask
// (order_util.hpp _is_buy_order), so the CancelAck's side+price are recorded
// here when the order is (re)inserted. Written on the clock alongside g_h2e.
std::vector<uint8_t>  g_h2side;       // harness id -> resting side (0 buy/1 sell)
std::vector<int64_t>  g_h2price;      // harness id -> resting price (ticks)

// Per-call aggressor + fill bookkeeping (the engine matches synchronously on
// the calling thread, so these are touched only by the active engine_on_* call).
id_type  g_aggr_engine_id = 0;   // engine id of the order being inserted (taker)
uint64_t g_aggr_seq       = 0;   // its harness sequence number
uint64_t g_filled         = 0;   // quantity it has filled so far this call

inline void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
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

inline uint64_t h2e(uint64_t hid) {
    return hid < g_h2e.size() ? g_h2e[hid] : 0;
}
inline uint64_t e2h(id_type eid) {
    return eid < g_e2h.size() ? g_e2h[eid] : 0;
}

inline void ensure_h2e(uint64_t hid) {
    if (hid >= g_h2e.size()) {
        size_t ns = std::max<size_t>(g_h2e.size() * 2, hid + 1);
        g_h2e.resize(ns, 0);
        g_h2side.resize(ns, 0);
        g_h2price.resize(ns, 0);
    }
}
inline void record_engine_id(id_type eid, uint64_t hid) {
    if (eid >= g_e2h.size())
        g_e2h.resize(std::max<size_t>(g_e2h.size() * 2, eid + 1), 0);
    g_e2h[eid] = hid;
}

// The execution callback the engine invokes (synchronously, on this thread)
// for every fill. Fires once per side; we emit the Trade on the maker side.
void exec_cb(callback_msg msg, id_type id1, id_type /*id2*/,
             double price, size_t size) {
    if (msg != callback_msg::fill)
        return;                        // we never use cancel/stop/advanced cbs
    if (id1 == g_aggr_engine_id) {
        // taker-side notification of this fill — the maker-side notification of
        // the same fill carries the resting id; count the fill once, there.
        return;
    }
    // maker side: id1 is the resting (maker) order's engine id.
    me_report_t r{};
    r.type            = ME_TRADE;
    r.sequence_number = g_aggr_seq;                       // aggressive order
    r.price_ticks     = static_cast<int64_t>(std::llround(price));  // maker price
    r.quantity        = static_cast<uint32_t>(size);
    r.maker_order_id  = e2h(id1);
    r.taker_order_id  = e2h(g_aggr_engine_id);
    push_report(r);
    g_filled += size;
}

// Keep the book covering `tick` (lazy grow for an out-of-range held-out seed).
inline void ensure_price_in_book(int64_t tick) {
    if (!g_mgmt) return;
    double p = static_cast<double>(tick);
    if (p < g_book->min_price() + 2.0)
        g_mgmt->grow_book_below(p - 64.0 > 1.0 ? p - 64.0 : 1.0);
    if (p > g_book->max_price() - 2.0)
        g_mgmt->grow_book_above(p + 64.0);
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;

    g_book = sob::SimpleOrderbook::BuildFactoryProxy<WholeTick>()
                 .create(BOOK_MIN, BOOK_MAX);
    g_mgmt = dynamic_cast<ManagementInterface*>(g_book);

    // Capacity pre-sizing (untimed) — the static-allocation parity a
    // fixed-array engine gets. Per-order writes stay on the clock.
    g_h2e.assign(1u << 21, 0);
    g_e2h.assign(1u << 21, 0);
    g_h2side.assign(1u << 21, 0);
    g_h2price.assign(1u << 21, 0);
}

void engine_shutdown(void) {
    if (g_book) {
        sob::SimpleOrderbook::Destroy(g_book);
        g_book = nullptr;
        g_mgmt = nullptr;
    }
    g_h2e.clear();
    g_e2h.clear();
    g_h2side.clear();
    g_h2price.clear();
}

// SimpleOrderbook's synchronous API has fully processed (and reported) every
// delivered order by the time the call returned — nothing is pending.
void engine_flush(void) {}

// Pre-build: capacity pre-sizing ONLY (untimed). No order is built, inserted,
// matched, or id-registered here — all of that is on the clock in engine_on_*.
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type == 0) {
        const new_order_t* o = static_cast<const new_order_t*>(msg);
        ensure_h2e(o->order_id);
    } else if (msg_type == 2) {
        const modify_t* m = static_cast<const modify_t*>(msg);
        ensure_h2e(m->order_id);
    }
    // engine id table is grown on the clock as ids are issued.
}

void engine_on_new_order(const new_order_t* o) {
    g_aggr_seq = o->sequence_number;
    g_filled   = 0;
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    ensure_price_in_book(o->price_ticks);

    // Reserve the engine id this insert will be assigned (the engine issues
    // ++_last_id; the next limit insert gets last_id()+1) so the maker/taker
    // reverse map is populated BEFORE the fill callbacks fire.
    id_type eid = g_book->last_id() + 1;
    g_aggr_engine_id = eid;
    record_engine_id(eid, o->order_id);

    id_type got = g_book->insert_limit_order(
        o->side == 0, static_cast<double>(o->price_ticks),
        static_cast<size_t>(o->quantity), exec_cb);
    // got == eid in the dense, stop-free id stream; remap defensively in case
    // the engine ever skipped an id.
    if (got != eid) record_engine_id(got, o->order_id);
    g_h2e[o->order_id]    = got;
    g_h2side[o->order_id] = o->side;            // resting side, for CancelAck
    g_h2price[o->order_id] = o->price_ticks;    // resting price, for CancelAck

    if (o->ioc) {
        // IOC: drop anything that did not fill. If quantity remains resting,
        // pull it and report the residual cancellation.
        if (g_filled < o->quantity) {
            g_book->pull_order(got);
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                     o->price_ticks,
                     static_cast<uint32_t>(o->quantity - g_filled));
        }
    }
    g_aggr_engine_id = 0;
}

void engine_on_cancel(const cancel_t* c) {
    id_type eid = h2e(c->order_id);
    // pull_order returns true iff the order was resting; false otherwise
    // (never seen / already filled / already pulled) — the engine's native
    // liveness check. Cancels never produce fills, but set the aggressor guard
    // so a stray fill cb (there is none) could not be misattributed.
    g_aggr_engine_id = 0;
    bool pulled = eid && g_book->pull_order(eid);
    if (pulled) {
        // CancelAck carries the resting order's side + price (the canonical
        // form hashes both); read them from the identity shadow recorded at
        // (re)insert, then clear the slot.
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
                 g_h2side[c->order_id], g_h2price[c->order_id], 0);
        g_h2e[c->order_id] = 0;
    } else {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    g_aggr_seq = m->sequence_number;
    g_filled   = 0;
    id_type old_eid = h2e(m->order_id);

    // Cancel + reinsert. pull_order tells us natively whether it was resting.
    if (!old_eid || !g_book->pull_order(old_eid)) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        g_aggr_engine_id = 0;
        return;
    }

    ensure_price_in_book(m->new_price_ticks);

    id_type eid = g_book->last_id() + 1;
    g_aggr_engine_id = eid;
    record_engine_id(eid, m->order_id);

    id_type got = g_book->insert_limit_order(
        m->side == 0, static_cast<double>(m->new_price_ticks),
        static_cast<size_t>(m->new_quantity), exec_cb);
    if (got != eid) record_engine_id(got, m->order_id);
    g_h2e[m->order_id]    = got;
    g_h2side[m->order_id] = m->side;            // new resting side
    g_h2price[m->order_id] = m->new_price_ticks; // new resting price

    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             m->side, m->new_price_ticks, m->new_quantity);
    g_aggr_engine_id = 0;
}

int64_t engine_query_best_bid(void) {
    double p = g_book->bid_price();              // 0.0 when no bids
    return p > 0.0 ? static_cast<int64_t>(std::llround(p)) : INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    double p = g_book->ask_price();              // 0.0 when no asks
    return p > 0.0 ? static_cast<int64_t>(std::llround(p)) : INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    double p = static_cast<double>(price_ticks);
    if (!g_book->is_valid_price(p))
        return 0;
    // Aggregated non-AON limit depth at this single price level. The engine's
    // depth maps are keyed by price; pull the entry for p from the right side.
    if (side == 0) {
        auto d = g_book->bid_depth(1u << 20);
        auto it = d.find(p);
        return it != d.end() ? it->second : 0;
    } else {
        auto d = g_book->ask_depth(1u << 20);
        auto it = d.find(p);
        return it != d.end() ? it->second : 0;
    }
}

// Batch delivery: process a run of messages in one cross-.so call. Same strict
// in-order semantics as one-at-a-time delivery (loops the per-message handlers,
// inlined under -O3) — removes only the per-message indirect-call dispatch.
void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const me_msg_t& mm = msgs[i];
        if (mm.type == 0)      engine_on_new_order(&mm.no);
        else if (mm.type == 1) engine_on_cancel(&mm.c);
        else                   engine_on_modify(&mm.md);
    }
}

}  // extern "C"
