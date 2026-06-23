/*
 * kautenja_adapter.cpp — Kautenja/limit-order-book behind the harness
 * matching_engine_api.h ABI.
 *
 * Engine native API. Kautenja's LOB (include/limit_order_book.hpp) is a
 * header-only price-time-priority book: a per-side BST of price `Limit` nodes
 * (vendor/binary-search-tree), each holding a FIFO doubly-linked list of
 * `Order` nodes (vendor/doubly-linked-list), plus a flat tsl::robin_map of
 * price -> Limit* per side and a std::unordered_map<UID,Order> for cancel-by-id.
 * LimitOrderBook exposes:
 *   limit(side, uid, qty, price)   — add a limit order; crosses & rests remainder
 *   cancel(uid)                    — remove a resting order by id
 *   has(uid) / get(uid)            — existence / lookup
 *   best_buy() / best_sell()       — best price per side (0 if that side empty)
 *   volume_buy(price)/volume_sell(price) — aggregate resting qty at a price level
 * Matching is triggered inside limit_buy/limit_sell: a crossing incoming order
 * is run through the opposite tree's market(order, did_fill) before the residual
 * (if any) rests. The book matches synchronously on the calling thread, so
 * engine_flush() is a no-op.
 *
 * Trades. The engine's match loop (LimitTree::market) reports a consumed maker
 * only by uid (via did_fill), carries no per-fill price/quantity, and — for the
 * LAST maker, when it is only partially consumed — does not fire the callback at
 * all. The harness Trade needs the maker's resting price + the fill quantity per
 * fill, in match order. So build.sh injects a one-line per-fill hook
 * `__kautenja_trade_hook(maker_uid, maker_price, fill_qty)` into market() at both
 * fill sites (the jxm35 pattern: a hook the engine should call but doesn't). The
 * adapter implements that hook and emits one ME_TRADE per call. Maker price is
 * the resting order's price (match->price); the aggressor's seq is threaded
 * through gCurSeq.
 *
 * Reports synthesised above the engine. The engine has no fill/ack/reject
 * callback that carries side+price, so OrderAck / CancelAck / ModifyAck /
 * CancelReject / ModifyReject are all synthesised here. IOC has no native
 * support: the order is submitted as a plain limit, matches what it can, and the
 * adapter cancels + drops the unfilled residual (emitting the residual
 * CancelAck) so it never rests. Modify has no native support either: it is
 * cancel + reinsert — the cancel half is adjudicated by the adapter's liveness
 * shadow (not resting -> ModifyReject), and the reinsert's crossing fills cross
 * with the modify message's seq (the reinsert loses queue priority), per the
 * harness contract.
 *
 * Price offset. Harness prices are signed int64 ticks where 0 is a valid limit
 * price; the engine's Price is uint64 and treats price 0 as the "market order"
 * sentinel (can_match short-circuits true on 0). Every price is therefore mapped
 * into a strictly-positive engine space with a fixed additive offset PX_OFF on
 * the way in and PX_OFF subtracted on the way out (best bid/ask, depth-at, Trade
 * maker price). The canonical workload's prices sit around the NVDA reference
 * (~33.5k ticks) and never approach this offset's magnitude, so the mapping is a
 * strictly-increasing bijection that preserves all price comparisons.
 *
 * Per-order liveness shadow. A flat array indexed by order id holds
 * {oid -> price(harness ticks), side, remaining, alive}. It is REQUIRED to
 * distinguish a resting order from a not-resting one so cancel/modify can
 * synthesise CancelAck/ModifyAck vs CancelReject/ModifyReject (the canonical
 * workload injects ~2% stale cancels/modifies), and to echo the resting order's
 * side/price on those acks (the engine surfaces neither on cancel). It is NOT
 * used for the audit queries — those go straight to the engine's live book
 * (best_buy/best_sell/volume_*), so a stale shadow can never fool the audit.
 *
 * Pin. build.sh resets the engine to its current HEAD
 * (88416a12a0b34b026cbf1d598823fd315a1f2dbf) and applies the single trade-hook
 * patch idempotently (git reset --hard first).
 */

#include <cstdint>
#include <vector>

#include "matching_engine_api.h"
#include "limit_order_book.hpp"

using LOB::LimitOrderBook;
using LOB::Side;
using LOB::Price;
using LOB::Quantity;
using LOB::UID;

#define HOT_INLINE __attribute__((always_inline, hot)) inline

namespace {

// ---------------------------------------------------------------------------
// Price offset. Map signed harness ticks into the engine's strictly-positive
// uint64 price space (engine price 0 == market sentinel; harness 0 is a valid
// limit price and prices can in principle be <= 0). PX_OFF dwarfs every price in
// the canonical workload (~33.5k ticks) yet leaves vast headroom below 2^63, so
// the map is a strictly-increasing bijection that preserves all comparisons.
// ---------------------------------------------------------------------------
constexpr int64_t PX_OFF = int64_t(1) << 30;

HOT_INLINE Price toEnginePrice(int64_t ticks) {
    return static_cast<Price>(ticks + PX_OFF);
}
HOT_INLINE int64_t fromEnginePrice(Price p) {
    return static_cast<int64_t>(p) - PX_OFF;
}

// ---------------------------------------------------------------------------
// Per-order liveness shadow (see header comment). 16 bytes, flat, id-indexed.
// ---------------------------------------------------------------------------
struct Shadow {
    int64_t  price = 0;      // harness ticks (NOT engine-offset)
    uint32_t remaining = 0;  // resting quantity
    uint8_t  side = 0;       // 0 = buy, 1 = sell
    bool     alive = false;
};

constexpr size_t kShadowInit = size_t(1) << 22;
std::vector<Shadow> gShadow;
Shadow* gShadowBase = nullptr;
size_t  gShadowCap = 0;

HOT_INLINE Shadow* shadowSlot(uint64_t oid) {
    if (oid >= gShadowCap) [[unlikely]] {
        gShadow.resize(oid + 1);
        gShadowBase = gShadow.data();
        gShadowCap = gShadow.size();
    }
    return gShadowBase + oid;
}

LimitOrderBook* gBook = nullptr;

const me_transport_t* gTransport = nullptr;
void* gSink = nullptr;

// Per-call context for the injected trade hook (taker = current aggressor).
uint64_t gCurSeq = 0;       // aggressor's sequence_number (goes on Trades)
uint64_t gTakerId = 0;      // aggressor's order_id
uint64_t gTakerFill = 0;    // qty the aggressor filled this call (drives IOC residual)

HOT_INLINE void cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

HOT_INLINE void emit(const me_report_t* r) {
    while (gTransport->push(gSink, r) == 0) [[unlikely]] {
        cpu_pause();
    }
}

HOT_INLINE void emitAck(uint8_t rtype, uint64_t seq, uint64_t oid,
                        uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type = rtype;
    r.side = side;
    r.sequence_number = seq;
    r.order_id = oid;
    r.price_ticks = price;
    r.quantity = qty;
    emit(&r);
}

}  // namespace

// ---------------------------------------------------------------------------
// Per-fill hook injected into LimitTree::market by build.sh. Called once per
// fill, in match order, with the consumed maker's uid, the maker's resting
// (engine-offset) price, and the quantity filled. Maps maker price back to
// harness ticks, emits one ME_TRADE, accumulates the taker's filled tally, and
// decrements the maker's liveness shadow.
// ---------------------------------------------------------------------------
extern "C" void __kautenja_trade_hook(uint64_t maker_uid, uint64_t maker_price,
                                      uint32_t fill_qty) {
    me_report_t r{};
    r.type = ME_TRADE;
    r.side = 0;
    r.sequence_number = gCurSeq;                              // aggressor's seq
    r.order_id = maker_uid;
    r.price_ticks = fromEnginePrice(static_cast<Price>(maker_price));  // maker resting price
    r.quantity = fill_qty;
    r.maker_order_id = maker_uid;
    r.taker_order_id = gTakerId;
    emit(&r);

    gTakerFill += fill_qty;

    Shadow* e = shadowSlot(maker_uid);
    uint32_t rem = e->remaining;
    rem = (rem >= fill_qty) ? uint32_t(rem - fill_qty) : 0u;
    e->remaining = rem;
    if (rem == 0) e->alive = false;
}

namespace {

// ---------------------------------------------------------------------------
// Per-message handlers (shared by the per-message ABI and engine_on_batch).
// ---------------------------------------------------------------------------

HOT_INLINE void onNewOrder(const new_order_t* o) {
    const uint64_t seq = o->sequence_number;
    const uint64_t oid = o->order_id;
    const uint8_t  side = o->side;
    const int64_t  price = o->price_ticks;
    const uint32_t qty = o->quantity;
    const uint8_t  ioc = o->ioc;

    // 1. OrderAck (the engine accepts the new order).
    emitAck(ME_ORDER_ACK, seq, oid, side, price, qty);

    // 2. Drive the engine. The injected trade hook reads gCurSeq/gTakerId and
    //    writes gTakerFill + the maker shadow as each fill crosses.
    gCurSeq = seq;
    gTakerId = oid;
    gTakerFill = 0;

    const Side sideE = (side == 0) ? Side::Buy : Side::Sell;
    gBook->limit(sideE, oid, qty, toEnginePrice(price));

    const uint64_t filled = gTakerFill;
    const uint32_t residual = (filled < qty) ? uint32_t(qty - filled) : 0;

    if (ioc != 0) [[unlikely]] {
        // IOC: the engine rested the unfilled residual (no native IOC). Cancel
        // it so it never rests, and emit one CancelAck for the dropped residual.
        if (residual > 0) {
            if (gBook->has(oid)) gBook->cancel(oid);
            emitAck(ME_CANCEL_ACK, seq, oid, side, price, residual);
        }
        return;
    }

    // GTC: the residual rests. Record it so future cancel/modify can find it.
    if (residual > 0) {
        Shadow* e = shadowSlot(oid);
        e->price = price;
        e->side = side;
        e->remaining = residual;
        e->alive = true;
    }
}

HOT_INLINE void onCancel(const cancel_t* c) {
    const uint64_t seq = c->sequence_number;
    const uint64_t oid = c->order_id;

    Shadow* e = (oid < gShadowCap) ? (gShadowBase + oid) : nullptr;
    if (e != nullptr && e->alive) {
        gBook->cancel(oid);                  // remove from the live book
        emitAck(ME_CANCEL_ACK, seq, oid, e->side, e->price, e->remaining);
        e->alive = false;
        e->remaining = 0;
    } else {
        // Not resting — already filled, already cancelled, or never seen.
        emitAck(ME_CANCEL_REJECT, seq, oid, 0, 0, 0);
    }
}

HOT_INLINE void onModify(const modify_t* m) {
    const uint64_t seq = m->sequence_number;
    const uint64_t oid = m->order_id;
    const int64_t  newPrice = m->new_price_ticks;
    const uint32_t newQty = m->new_quantity;

    Shadow* e = (oid < gShadowCap) ? (gShadowBase + oid) : nullptr;
    if (e == nullptr || !e->alive) {
        // Not resting — a stale modify. Reject.
        emitAck(ME_MODIFY_REJECT, seq, oid, 0, 0, 0);
        return;
    }

    const uint8_t side = e->side;

    // Cancel half (the order is resting): remove it from the book.
    gBook->cancel(oid);
    e->alive = false;
    e->remaining = 0;

    // ModifyAck.
    emitAck(ME_MODIFY_ACK, seq, oid, side, newPrice, newQty);

    // Reinsert at the new price/quantity (loses queue priority). Crossing fills
    // emit ME_TRADE with the modify message's seq.
    gCurSeq = seq;
    gTakerId = oid;
    gTakerFill = 0;

    const Side sideE = (side == 0) ? Side::Buy : Side::Sell;
    gBook->limit(sideE, oid, newQty, toEnginePrice(newPrice));

    const uint64_t filled = gTakerFill;
    const uint32_t residual = (filled < newQty) ? uint32_t(newQty - filled) : 0;

    e = shadowSlot(oid);   // limit() may have grown the shadow via the hook
    if (residual > 0) {
        e->price = newPrice;
        e->side = side;
        e->remaining = residual;
        e->alive = true;
    } else {
        e->alive = false;
        e->remaining = 0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported ABI.
// ---------------------------------------------------------------------------

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    gTransport = transport;
    gSink = report_sink;
    gCurSeq = 0;
    gTakerId = 0;
    gTakerFill = 0;

    gShadow.assign(kShadowInit, Shadow{});
    gShadowBase = gShadow.data();
    gShadowCap = gShadow.size();

    delete gBook;
    gBook = new LimitOrderBook();
}

void engine_shutdown(void) {
    delete gBook;
    gBook = nullptr;
    gShadow.clear();
    gShadow.shrink_to_fit();
    gShadowBase = nullptr;
    gShadowCap = 0;
}

void engine_on_new_order(const new_order_t* order) { onNewOrder(order); }
void engine_on_cancel(const cancel_t* cancel) { onCancel(cancel); }
void engine_on_modify(const modify_t* modify) { onModify(modify); }

// Synchronous matcher: engine_on_* has already produced every report.
void engine_flush(void) {}

int64_t engine_query_best_bid(void) {
    // Engine best_buy() returns 0 when the buy side is empty (no bids).
    if (gBook->count_buy() == 0) return INT64_MIN;
    return fromEnginePrice(gBook->best_buy());
}

int64_t engine_query_best_ask(void) {
    if (gBook->count_sell() == 0) return INT64_MAX;
    return fromEnginePrice(gBook->best_sell());
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const Price p = toEnginePrice(price_ticks);
    return (side == 0) ? gBook->volume_buy(p) : gBook->volume_sell(p);
}

void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const me_msg_t& m = msgs[i];
        switch (m.type) {
            case 0: onNewOrder(&m.no); break;
            case 1: onCancel(&m.c); break;
            case 2: onModify(&m.md); break;
            default: break;
        }
    }
}

}  // extern "C"
