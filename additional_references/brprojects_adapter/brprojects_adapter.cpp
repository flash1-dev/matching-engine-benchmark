/* brprojects_adapter.cpp — brprojects/Limit-Order-Book behind the harness
 * matching_engine_api.h ABI.
 *
 * Engine. brprojects/Limit-Order-Book ("Central Limit Order Book") is a
 * price-time-priority limit order book: each side is an AVL tree of price
 * levels (Limit), each level a FIFO doubly-linked list of Order nodes, with an
 * order_id->Order* hash map. The public API is Book::addLimitOrder /
 * cancelLimitOrder / modifyLimitOrder (and market / stop / stop-limit variants
 * the canonical workload never uses). A new limit order is first crossed
 * against the opposite book (limitOrderAsMarketOrder -> marketOrderHelper) and
 * the residual rests; cancel unlinks the order; the canonical workload uses
 * only AddLimit / Cancel / Modify (+ an IOC flag on news).
 *
 * Why a patch. The engine has NO fill/trade notification of any kind — its
 * matching loop (Book::marketOrderHelper) executes and deletes resting orders
 * silently and only bumps an int counter. The harness requires one Trade per
 * fill carrying the maker's resting price + maker/taker ids, which is matcher
 * information only the engine sees. Re-deriving fills in the adapter would mean
 * reimplementing matching (forbidden by the adapter mandate), so build.sh
 * applies a MINIMAL engine-source patch: a per-fill hook
 * (g_brp_fill_hook(taker_id, maker_id, maker_price, qty)) is invoked at the two
 * fill sites inside marketOrderHelper. This is the jxm35 pattern (inject the
 * per-fill callback the engine never calls). The matching logic itself is
 * untouched. See build.sh for the exact str.replace.
 *
 * Reports synthesised in the adapter. The engine reports nothing else either
 * (no accept, no cancel result), so OrderAck / CancelAck / ModifyAck and the
 * Trade are all emitted here; CancelReject / ModifyReject come from a per-order
 * liveness shadow (the engine's cancelLimitOrder silently no-ops a not-resting
 * id, so the adapter must adjudicate "is this order resting?" itself — the
 * minimal liveness shadow the brief permits). Modify = cancel + reinsert
 * (adapter-driven), so the reinsert re-crosses the book and its fills are
 * reported, matching the harness contract; the engine's own modifyLimitOrder
 * (which re-appends WITHOUT re-matching) is therefore NOT used.
 *
 * Trade fields. maker price = the resting level's price (passed by the hook
 * from bookEdge->getLimitPrice()); sequence_number = the aggressive message's
 * seq (threaded through g_seq); maker/taker ids from the hook. Fills arrive in
 * match order (best price first, FIFO within a level), exactly as the harness
 * wants.
 *
 * Shadow / queries. A flat array indexed by order id holds {price, side,
 * remaining, alive}. It supplies the side/price echoed on acks (the engine
 * tells the adapter nothing) and adjudicates Cancel/Modify reject (the
 * engine's cancelLimitOrder silently no-ops a not-resting id). It is NOT used
 * to answer engine_query_best_bid / best_ask / depth_at: those forward
 * straight to the engine's own book — Book::getHighestBuy() / getLowestSell()
 * (null when that side is empty) and Book::searchLimitMaps(price, side)
 * ->getTotalVolume() (null Limit* -> 0 depth) — so a real book-state bug in
 * the engine's own AVL-edge bookkeeping would surface here rather than being
 * masked by an independently-correct shadow. The engine's own orderMap/AVL
 * trees are the matcher; the shadow only mirrors what the engine itself never
 * reports, kept in lockstep by the fill hook (decrement maker remaining) and
 * the new/cancel/modify handlers.
 *
 * Types. Engine order ids and prices are `int` (32-bit signed). The canonical
 * workload's ids are dense 1..N (<=300k here) and prices are small positive
 * ticks (mid ~33504, depth <=799 ticks out), so they fit `int` with vast
 * headroom; the adapter passes the 64-bit ABI values through int() and keeps
 * its own 64-bit shadow for reporting.
 *
 * Pin. brprojects/Limit-Order-Book @ af6e5349874649fe196bd6c26653d357f5a751f2
 * (current HEAD). build.sh git-resets to the pin, applies the fill-hook patch,
 * and compiles the three matcher TUs + this adapter into one .so.
 */

#include <cstdint>
#include <vector>

#include "matching_engine_api.h"

#include "Book.hpp"
#include "Limit.hpp"
#include "Order.hpp"

// ---------------------------------------------------------------------------
// Engine fill hook (injected by build.sh into Book::marketOrderHelper).
// Invoked once per fill with the taker (aggressor) id, the maker (resting) id,
// the maker's resting price, and the filled quantity — all ints, matching the
// engine's native int order-id / price types.
// ---------------------------------------------------------------------------
extern "C" void (*g_brp_fill_hook)(int taker_id, int maker_id,
                                   int maker_price, int qty);

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

#define HOT_INLINE __attribute__((always_inline, hot)) inline

namespace {

// --- Per-order shadow ------------------------------------------------------
// 16 bytes packed: int64 price + uint32 remaining + uint8 side + bool alive.
struct Shadow {
    int64_t  price     = 0;
    uint32_t remaining = 0;
    uint8_t  side      = 0;   // 0 = buy, 1 = sell
    bool     alive     = false;
};

constexpr size_t kShadowInit = size_t(1) << 19;   // generous; workload ~300k ids

std::vector<Shadow> g_shadow;
Shadow*             g_shadow_base = nullptr;
size_t              g_shadow_cap  = 0;

Book*                 g_book      = nullptr;
const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// Per-call context read by the fill hook.
uint64_t g_seq        = 0;   // aggressive message's sequence number
uint64_t g_taker_fill = 0;   // qty the aggressor filled this call

HOT_INLINE Shadow* shadow_slot(uint64_t oid) {
    if (oid >= g_shadow_cap) [[unlikely]] {
        g_shadow.resize(oid + 1);
        g_shadow_base = g_shadow.data();
        g_shadow_cap  = g_shadow.size();
    }
    return g_shadow_base + oid;
}

HOT_INLINE void emit(const me_report_t* r) {
    while (g_transport->push(g_sink, r) == 0) [[unlikely]] {
        cpu_pause();
    }
}

HOT_INLINE void emit_ack(uint8_t rtype, uint64_t seq, uint64_t oid,
                         uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = rtype;
    r.side            = side;
    r.sequence_number = seq;
    r.order_id        = oid;
    r.price_ticks     = price;
    r.quantity        = qty;
    emit(&r);
}

// The engine fill hook. Emits one Trade and decrements the maker's shadow.
void on_fill(int taker_id, int maker_id, int maker_price, int qty) {
    me_report_t r{};
    r.type            = ME_TRADE;
    r.side            = 0;
    r.sequence_number = g_seq;
    r.order_id        = uint64_t(uint32_t(maker_id));
    r.price_ticks     = maker_price;                 // maker's resting price
    r.quantity        = uint32_t(qty);
    r.maker_order_id  = uint64_t(uint32_t(maker_id));
    r.taker_order_id  = uint64_t(uint32_t(taker_id));
    emit(&r);

    g_taker_fill += uint32_t(qty);

    // Maker fully or partially consumed: keep the shadow in lockstep.
    Shadow* m = shadow_slot(uint64_t(uint32_t(maker_id)));
    uint32_t rem = m->remaining;
    rem = (rem >= uint32_t(qty)) ? uint32_t(rem - uint32_t(qty)) : 0u;
    m->remaining = rem;
    if (rem == 0) m->alive = false;
}

// --- Hot-path handlers (shared by per-message ABI and engine_on_batch) ------

HOT_INLINE void do_new(const new_order_t* o) {
    const uint64_t seq   = o->sequence_number;
    const uint64_t oid   = o->order_id;
    const uint8_t  side  = o->side;
    const int64_t  price = o->price_ticks;
    const uint32_t qty   = o->quantity;
    const uint8_t  ioc   = o->ioc;

    // 1. OrderAck (engine has no accept notification of its own).
    emit_ack(ME_ORDER_ACK, seq, oid, side, price, qty);

    // 2. Drive the engine. The fill hook reads g_seq, emits Trades, and writes
    //    g_taker_fill / decrements maker shadows.
    g_seq        = seq;
    g_taker_fill = 0;
    g_book->addLimitOrder(int(oid), side == 0 /*buy*/, int(qty), int(price));

    const uint32_t residual = (g_taker_fill < qty)
                                  ? uint32_t(qty - g_taker_fill) : 0u;

    if (ioc != 0) [[unlikely]] {
        // IOC: the residual must not rest. addLimitOrder rested it (the engine
        // has no IOC), so cancel it back out and report the residual CancelAck.
        if (residual > 0) {
            g_book->cancelLimitOrder(int(oid));
            emit_ack(ME_CANCEL_ACK, seq, oid, side, price, residual);
        }
        // IOC never leaves a resting order; shadow slot stays dead.
        return;
    }

    // GTC: shadow tracks the resting remainder (0 if fully filled).
    Shadow* e = shadow_slot(oid);
    e->price     = price;
    e->side      = side;
    e->remaining = residual;
    e->alive     = (residual > 0);
}

HOT_INLINE void do_cancel(const cancel_t* c) {
    const uint64_t seq = c->sequence_number;
    const uint64_t oid = c->order_id;

    Shadow* e = (oid < g_shadow_cap) ? (g_shadow_base + oid) : nullptr;
    if (e == nullptr || !e->alive) [[unlikely]] {
        // Not resting (never seen / already filled / already cancelled).
        emit_ack(ME_CANCEL_REJECT, seq, oid, 0, 0, 0);
        return;
    }

    g_book->cancelLimitOrder(int(oid));
    emit_ack(ME_CANCEL_ACK, seq, oid, e->side, e->price, e->remaining);
    e->alive = false;
}

HOT_INLINE void do_modify(const modify_t* m) {
    const uint64_t seq      = m->sequence_number;
    const uint64_t oid      = m->order_id;
    const int64_t  newPrice = m->new_price_ticks;
    const uint32_t newQty   = m->new_quantity;

    Shadow* e = (oid < g_shadow_cap) ? (g_shadow_base + oid) : nullptr;
    if (e == nullptr || !e->alive) [[unlikely]] {
        emit_ack(ME_MODIFY_REJECT, seq, oid, 0, 0, 0);
        return;
    }

    const uint8_t side = e->side;

    // Cancel + reinsert (harness modify semantics: loses queue priority, may
    // re-cross). Pull the order out of the engine, then re-add at new
    // price/qty so the reinsert matches against the book.
    g_book->cancelLimitOrder(int(oid));
    e->alive = false;

    emit_ack(ME_MODIFY_ACK, seq, oid, side, newPrice, newQty);

    g_seq        = seq;
    g_taker_fill = 0;
    g_book->addLimitOrder(int(oid), side == 0 /*buy*/, int(newQty), int(newPrice));

    const uint32_t residual = (g_taker_fill < newQty)
                                  ? uint32_t(newQty - g_taker_fill) : 0u;

    e = shadow_slot(oid);   // reinsert may have grown the store (cold)
    e->price     = newPrice;
    e->side      = side;
    e->remaining = residual;
    e->alive     = (residual > 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------
extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_seq       = 0;
    g_taker_fill = 0;

    g_brp_fill_hook = &on_fill;

    g_shadow.assign(kShadowInit, Shadow{});
    g_shadow_base = g_shadow.data();
    g_shadow_cap  = g_shadow.size();

    delete g_book;
    g_book = new Book();
}

void engine_shutdown(void) {
    delete g_book;
    g_book = nullptr;
    g_brp_fill_hook = nullptr;
    g_shadow.clear();
    g_shadow.shrink_to_fit();
    g_shadow_base = nullptr;
    g_shadow_cap  = 0;
}

void engine_on_new_order(const new_order_t* order) { do_new(order); }
void engine_on_cancel(const cancel_t* cancel)      { do_cancel(cancel); }
void engine_on_modify(const modify_t* modify)      { do_modify(modify); }

void engine_flush(void) { /* synchronous matcher: nothing deferred */ }

int64_t engine_query_best_bid(void) {
    // Forward to the engine's own AVL-edge cache: nullptr means the buy side
    // is empty (Book's ctor initialises highestBuy = nullptr; deleteLimit's
    // updateBookEdgeRemove keeps it live on every removal).
    Limit* l = g_book->getHighestBuy();
    return l ? int64_t(l->getLimitPrice()) : INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    Limit* l = g_book->getLowestSell();
    return l ? int64_t(l->getLimitPrice()) : INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    // searchLimitMaps does a plain map::find (no default-insertion) and
    // returns nullptr for a price with no resting limit on that side; a live
    // Limit's getTotalVolume() is the engine's own running total (incremented
    // on Limit::append, decremented on both a fill and a cancel — see
    // Order::cancel() / partiallyFillTotalVolume), i.e. the current resting
    // quantity, not a cumulative traded volume.
    Limit* l = g_book->searchLimitMaps(int(price_ticks), side == 0 /*buy*/);
    return l ? uint64_t(l->getTotalVolume()) : 0;
}

void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const me_msg_t& m = msgs[i];
        switch (m.type) {
            case 0: do_new(&m.no);    break;
            case 1: do_cancel(&m.c);  break;
            case 2: do_modify(&m.md); break;
            default: break;
        }
    }
}

}  // extern "C"
