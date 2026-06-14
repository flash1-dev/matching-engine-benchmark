/*
 * piyush_adapter.cpp — PIYUSH-KUMAR1809/order-matching-engine behind the
 * matching_engine_api.h harness ABI.
 *
 * Drives the engine's StandardMatchingStrategy + OrderBook inline (no Exchange
 * worker thread): the matching code path is identical in either case, and
 * inline removes thread-sync races that would otherwise confound the audit.
 *
 * The engine emits only Trade events (and no Ack/Reject types at all), so the
 * adapter synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
 * ModifyReject around the engine. A per-order shadow (oid -> {price, side,
 * remaining qty, alive}) tracks order state for the synthesised reports;
 * the trades vector match() fills keeps it current.
 *
 * IOC: piyush has no IOC type. The adapter submits the order as Limit, then
 * if any residual remains after matching, calls book.cancelOrder + emits a
 * CancelAck for the residual quantity.
 *
 * MODIFY: cancel + reinsert at new price/qty, mirroring the harness contract.
 */
#include "matching_engine_api.h"

#include "OrderBook.hpp"
#include "MatchingStrategy.hpp"
#include "Order.hpp"

#include <cstdint>
#include <algorithm>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;
OrderBook*            g_book      = nullptr;
StandardMatchingStrategy g_strategy;

// Per-order shadow needed because:
//  - engine emits no OrderAck / CancelAck / ModifyAck — adapter must synth all
//    of them, and CancelAck needs to echo the order's side and price after the
//    order has already been removed from the book.
//  - engine has no IOC support — adapter must detect residual and cancel it.
//  - successful cancel vs cancel-of-unknown is distinguished only by checking
//    the shadow (piyush's book.cancelOrder returns void).
struct Shadow {
    int64_t  price;
    uint8_t  side;       // 0 = buy, 1 = sell
    uint32_t remaining;  // current open quantity
    bool     alive;
};
/* Harness order ids are dense and 1-based, so a flat vector indexed by
 * order_id holds the shadow — no per-insert node allocation on the timed
 * path; a never-rested id is a zero slot (alive=false), the same reject
 * outcome a map miss produced. The engine itself indexes these exact ids
 * with a flat preallocated vector (OrderBook::idToLocation), proving the
 * pattern. Sized in engine_init; engine_prebuild grows it if ever needed. */
std::vector<Shadow> g_shadow;

/* Bounds-checked flat-vector lookup; nullptr / alive=false = not resting. */
inline Shadow* find_order(uint64_t ext_id) {
    return ext_id < g_shadow.size() ? &g_shadow[ext_id] : nullptr;
}

// Reused trades buffer: avoids a per-message heap malloc+free that a fresh
// local std::vector would incur on the timed hot path. Reserved once in
// engine_init, cleared (not reallocated) at each match site. Mirrors the
// reserved fills buffer in jxm35_adapter / robaho_adapter.
std::vector<Trade> g_trades;

inline void emit(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin */ }
}

inline void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
                     uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    emit(r);
}

// Drain the strategy's trades vector into Trade reports, attributing each fill
// to the taker (whose seq is taker_seq). Also updates the maker's remaining
// quantity in the shadow and marks fully-filled orders dead.
inline uint64_t flush_trades(std::vector<Trade>& trades, uint64_t taker_seq) {
    uint64_t taker_filled = 0;
    for (const auto& t : trades) {
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = taker_seq;
        r.price_ticks     = t.price;
        r.quantity        = t.quantity;
        r.maker_order_id  = t.makerOrderId;
        r.taker_order_id  = t.takerOrderId;
        emit(r);
        taker_filled += t.quantity;
        if (Shadow* s = find_order(t.makerOrderId)) {
            if (s->remaining >= t.quantity) {
                s->remaining -= t.quantity;
            } else {
                s->remaining = 0;
            }
            if (s->remaining == 0) s->alive = false;
        }
    }
    trades.clear();
    return taker_filled;
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_book      = new OrderBook();
    /* resize (not reserve): zero-fills the slots (alive=false) and faults the
     * pages in, both untimed. engine_prebuild grows past this if needed. */
    g_shadow.clear();
    g_shadow.resize(1u << 21);    // ~2M entries headroom
    g_trades.reserve(64);         // matches the jxm35/robaho fills buffers
}

/* Pre-build hook: only used to size the flat shadow vector to the workload's
 * id range, outside the timed window. */
void engine_prebuild(uint8_t msg_type, const void* msg) {
    if (msg_type != 0) return;
    const new_order_t* o = static_cast<const new_order_t*>(msg);
    if (o->order_id >= g_shadow.size())
        g_shadow.resize(std::max<size_t>(g_shadow.size() * 2,
                                         static_cast<size_t>(o->order_id) + 1));
}

void engine_shutdown(void) {
    delete g_book;
    g_book = nullptr;
    g_shadow.clear();
    g_trades.clear();
}

void engine_flush(void) {
    // Inline matching means there is nothing in flight — every engine_on_* call
    // ran the matcher synchronously before returning. No barrier needed.
}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    Order order{};
    order.id            = o->order_id;
    order.clientOrderId = o->sequence_number;
    order.symbolId      = 0;
    order.side          = (o->side == 0) ? OrderSide::Buy : OrderSide::Sell;
    order.type          = OrderType::Limit;
    order.price         = static_cast<Price>(o->price_ticks);
    order.quantity      = o->quantity;
    order.active        = true;

    g_trades.clear();
    g_strategy.match(*g_book, order, g_trades);
    uint64_t filled = flush_trades(g_trades, o->sequence_number);

    const uint32_t residual = (filled < o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - filled)
                                  : 0u;

    if (o->ioc) {
        if (residual > 0) {
            // piyush has no IOC: the matcher rested any residual. Cancel it and
            // emit the harness's IOC CancelAck.
            g_book->cancelOrder(o->order_id);
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        // The order's lifecycle ends here either way; not added to shadow.
        return;
    }

    // GTC. Insert into shadow at the order's current state.
    if (residual > 0) {
        g_shadow[o->order_id] = { o->price_ticks, o->side, residual, true };
    } else {
        // Fully filled on submit — alive=false so subsequent cancel/modify reject.
        g_shadow[o->order_id] = { o->price_ticks, o->side, 0, false };
    }
}

void engine_on_cancel(const cancel_t* c) {
    Shadow* s = find_order(c->order_id);
    if (!s || !s->alive) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    g_book->cancelOrder(c->order_id);
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             s->side, s->price, s->remaining);
    s->alive = false;
}

void engine_on_modify(const modify_t* m) {
    Shadow* s = find_order(m->order_id);
    if (!s || !s->alive) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint8_t side = s->side;
    g_book->cancelOrder(m->order_id);
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    // Re-insert at the new price/qty. Matching at the new price may produce
    // crossing trades; those trades carry the modify message's seq.
    Order order{};
    order.id            = m->order_id;
    order.clientOrderId = m->sequence_number;
    order.symbolId      = 0;
    order.side          = (side == 0) ? OrderSide::Buy : OrderSide::Sell;
    order.type          = OrderType::Limit;
    order.price         = static_cast<Price>(m->new_price_ticks);
    order.quantity      = m->new_quantity;
    order.active        = true;

    g_trades.clear();
    g_strategy.match(*g_book, order, g_trades);
    const uint64_t filled = flush_trades(g_trades, m->sequence_number);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    *s = { m->new_price_ticks, side, residual, residual > 0 };
}

int64_t engine_query_best_bid(void) {
    Price b = g_book->getBestBid();
    // piyush returns 0 when no bids (not −1). Translate to the harness's
    // "no bids" sentinel (INT64_MIN); see api/matching_engine_api.h.
    if (b == 0 && !g_book->getBidMask().test(0)) return INT64_MIN;
    return static_cast<int64_t>(b);
}

int64_t engine_query_best_ask(void) {
    Price a = g_book->getBestAsk();
    if (a < 0) return INT64_MAX;
    return static_cast<int64_t>(a);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    if (price_ticks < 0 || price_ticks >= OrderBook::MAX_PRICE) return 0;
    const auto& level = g_book->getLevel(
        static_cast<Price>(price_ticks),
        (side == 0) ? OrderSide::Buy : OrderSide::Sell);
    uint64_t total = 0;
    for (const auto& o : level.orders) {
        if (o.active) total += o.quantity;
    }
    return total;
}

}  // extern "C"
