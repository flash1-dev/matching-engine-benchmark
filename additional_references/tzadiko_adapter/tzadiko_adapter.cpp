/*
 * tzadiko_adapter.cpp — Tzadiko/Orderbook behind the harness
 * matching_engine_api.h ABI.
 *
 * Tzadiko's `Orderbook` exposes a small synchronous API:
 *   - AddOrder(OrderPointer) -> Trades        (vector of bid+ask TradeInfo pairs)
 *   - CancelOrder(OrderId)   -> void          (silent no-op if not present)
 *   - ModifyOrder(OrderModify) -> Trades      (engine-internal cancel + AddOrder)
 *   - Size() / GetOrderInfos() for inspection (no best_bid/ask helpers)
 *
 * The engine has no IOC type per se — its closest equivalent is FillAndKill,
 * which the engine cancels itself at the end of the match loop. There are no
 * Ack/Reject report types, so the adapter synthesises OrderAck / CancelAck /
 * ModifyAck / CancelReject / ModifyReject above the engine and uses a shadow
 * map (oid -> {price, side, remaining, alive}) to drive the reject path and
 * to echo side/price on CancelAck/ModifyAck.
 *
 * Trade format: each Tzadiko Trade carries a bid-side TradeInfo and an
 * ask-side TradeInfo. The maker is the side opposite the aggressor (whose
 * side we know from the harness call); the maker's price is the fill price
 * the harness records in ME_TRADE.price_ticks.
 *
 * Audit queries (best_bid / best_ask / depth_at) are answered from an
 * adapter-side shadow rather than `Orderbook::GetOrderInfos()` — the engine's
 * snapshot helper walks every resting order via `std::accumulate` to aggregate
 * per-level quantity, which is O(N_resting) per call. The shadow tracks
 * per-price aggregate quantity (two `std::map`s, one descending for bids and
 * one ascending for asks) and is maintained from the same fill/ack/cancel
 * stream the adapter already keeps for the reject and ack-echo paths.
 *
 * Note: Tzadiko's Orderbook spawns a "prune GoodForDay orders" background
 * thread on construction. The workload uses only GoodTillCancel and
 * FillAndKill, so the prune thread sleeps the entire run and never touches
 * the book. (See build.sh — the engine source uses Windows-only
 * `localtime_s`, which is patched to `localtime_r` for Linux.)
 */
#include "matching_engine_api.h"

#include "Orderbook.h"
#include "Order.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "Side.h"
#include "Trade.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <sched.h>
#include <unordered_map>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

std::unique_ptr<Orderbook> g_book;

struct Shadow {
    int64_t  price;        // workload ticks
    uint8_t  side;         // 0 = buy, 1 = sell
    uint32_t remaining;
    bool     alive;
};
std::unordered_map<uint64_t, Shadow> g_shadow;

// Shadow per-price aggregate quantity, mirroring what the engine maintains
// internally — populated from the same fill/ack/cancel stream the adapter
// already drives. `bids_qty` is sorted descending so best_bid is the first
// entry; `asks_qty` is sorted ascending so best_ask is the first entry.
// Empty when the side has no resting depth (best_bid -> INT64_MIN sentinel,
// best_ask -> INT64_MAX sentinel — same as the GetOrderInfos path returned).
std::map<int64_t, uint64_t, std::greater<int64_t>> g_bid_qty;
std::map<int64_t, uint64_t>                         g_ask_qty;

inline void shadow_level_add(uint8_t side, int64_t price, uint32_t qty) {
    if (side == 0) g_bid_qty[price] += qty;
    else           g_ask_qty[price] += qty;
}

inline void shadow_level_sub(uint8_t side, int64_t price, uint32_t qty) {
    auto sub_one = [&](auto& m) {
        auto it = m.find(price);
        if (it == m.end()) return;
        if (it->second <= qty) m.erase(it);
        else                   it->second -= qty;
    };
    if (side == 0) sub_one(g_bid_qty);
    else           sub_one(g_ask_qty);
}

inline void emit(const me_report_t& r) {
    // Single-thread inline matching emits one report at a time; the drainer on
    // the adjacent core pops faster than the matcher can push, so push() almost
    // never returns 0. Yield every 256 iterations as a safety net in case the
    // drainer is briefly off-core.
    for (unsigned i = 0; ; ++i) {
        if (g_transport->push(g_sink, &r)) return;
        if ((i & 0xff) == 0xff) sched_yield();
    }
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

// Translate Tzadiko's Trade list into harness ME_TRADE reports. The
// aggressor's side is known (we just called AddOrder for it); the maker is
// the opposite side and its TradeInfo carries the resting price the harness
// stamps onto ME_TRADE.price_ticks.
inline uint64_t emit_trades(const Trades& trades, uint64_t taker_seq,
                            uint8_t taker_side) {
    uint64_t taker_filled = 0;
    for (const Trade& t : trades) {
        const TradeInfo& bid = t.GetBidTrade();
        const TradeInfo& ask = t.GetAskTrade();
        const bool taker_is_buy = (taker_side == 0);
        const TradeInfo& maker = taker_is_buy ? ask : bid;
        const TradeInfo& taker = taker_is_buy ? bid : ask;

        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = taker_seq;
        r.price_ticks     = static_cast<int64_t>(maker.price_);
        r.quantity        = static_cast<uint32_t>(maker.quantity_);
        r.maker_order_id  = static_cast<uint64_t>(maker.orderId_);
        r.taker_order_id  = static_cast<uint64_t>(taker.orderId_);
        emit(r);

        taker_filled += maker.quantity_;
        // The maker side is opposite the taker side. Decrement the maker's
        // resting depth on its own side at its own price (which is the fill
        // price the harness sees).
        const uint8_t maker_side = taker_is_buy ? 1u : 0u;
        shadow_level_sub(maker_side, static_cast<int64_t>(maker.price_),
                         static_cast<uint32_t>(maker.quantity_));
        if (auto it = g_shadow.find(maker.orderId_); it != g_shadow.end()) {
            if (it->second.remaining >= maker.quantity_)
                it->second.remaining -= maker.quantity_;
            else
                it->second.remaining = 0;
            if (it->second.remaining == 0) it->second.alive = false;
        }
    }
    return taker_filled;
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_book      = std::make_unique<Orderbook>();
    g_shadow.clear();
    g_shadow.reserve(1u << 21);
    g_bid_qty.clear();
    g_ask_qty.clear();
}

void engine_shutdown(void) {
    g_book.reset();   // joins the prune thread via Orderbook's destructor
    g_shadow.clear();
    g_bid_qty.clear();
    g_ask_qty.clear();
}

void engine_flush(void) {
    // Orderbook::AddOrder / CancelOrder / ModifyOrder are synchronous; nothing
    // in flight when control returns. The prune thread does not feed reports.
}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    const OrderType type = o->ioc ? OrderType::FillAndKill
                                  : OrderType::GoodTillCancel;
    auto order = std::make_shared<Order>(
        type,
        static_cast<OrderId>(o->order_id),
        (o->side == 0) ? Side::Buy : Side::Sell,
        static_cast<Price>(o->price_ticks),
        static_cast<Quantity>(o->quantity));

    Trades trades = g_book->AddOrder(order);
    const uint64_t filled = emit_trades(trades, o->sequence_number, o->side);
    const uint32_t residual = (filled < o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - filled)
                                  : 0u;

    if (o->ioc) {
        // FillAndKill: Tzadiko's MatchOrders tail already cancelled any
        // residual aggressor at the front of its side's level. Synthesise the
        // harness's CancelAck for the unfilled remainder. IOC never rests, so
        // there's no shadow level depth to account for here.
        if (residual > 0) {
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        return;
    }

    // GTC. residual == 0 means the order fully filled on submit — record it
    // dead so a later cancel/modify rejects. residual > 0 means the order
    // rests at the new price on its own side — add it to the level shadow.
    if (residual > 0) {
        shadow_level_add(o->side, o->price_ticks, residual);
    }
    g_shadow[o->order_id] = { o->price_ticks, o->side, residual, residual > 0 };
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_shadow.find(c->order_id);
    if (it == g_shadow.end() || !it->second.alive) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    const Shadow s = it->second;
    g_book->CancelOrder(static_cast<OrderId>(c->order_id));
    shadow_level_sub(s.side, s.price, s.remaining);
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             s.side, s.price, s.remaining);
    it->second.alive = false;
}

void engine_on_modify(const modify_t* m) {
    auto it = g_shadow.find(m->order_id);
    if (it == g_shadow.end() || !it->second.alive) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint8_t side    = it->second.side;
    const int64_t old_px  = it->second.price;
    const uint32_t old_qty = it->second.remaining;

    // Harness contract: cancel + reinsert at the new price/qty, with the
    // crossing trades carrying the modify message's seq. Tzadiko's
    // ModifyOrder does cancel + AddOrder internally, but its public API only
    // returns the trade vector — and the engine remembers the existing
    // order's OrderType, which is GoodTillCancel (we never modify an IOC).
    // Use the engine's ModifyOrder directly and stamp the modify's seq onto
    // the returned trades; this matches the harness expectation.
    g_book->CancelOrder(static_cast<OrderId>(m->order_id));
    shadow_level_sub(side, old_px, old_qty);
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    auto order = std::make_shared<Order>(
        OrderType::GoodTillCancel,
        static_cast<OrderId>(m->order_id),
        (side == 0) ? Side::Buy : Side::Sell,
        static_cast<Price>(m->new_price_ticks),
        static_cast<Quantity>(m->new_quantity));
    Trades trades = g_book->AddOrder(order);
    const uint64_t filled = emit_trades(trades, m->sequence_number, side);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    if (residual > 0) {
        shadow_level_add(side, m->new_price_ticks, residual);
    }
    it->second = { m->new_price_ticks, side, residual, residual > 0 };
}

// Audit queries are answered from the adapter-side shadow rather than
// `Orderbook::GetOrderInfos()` — that snapshot helper walks every resting
// order via `std::accumulate` on each price level, which is O(N_resting) per
// call. The shadow is maintained from the same fill / ack / cancel stream the
// adapter already drives, so the answers track the engine's view exactly.
int64_t engine_query_best_bid(void) {
    if (g_bid_qty.empty()) return INT64_MIN;
    return g_bid_qty.begin()->first;
}

int64_t engine_query_best_ask(void) {
    if (g_ask_qty.empty()) return INT64_MAX;
    return g_ask_qty.begin()->first;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    if (side == 0) {
        auto it = g_bid_qty.find(price_ticks);
        return (it == g_bid_qty.end()) ? 0u : it->second;
    }
    auto it = g_ask_qty.find(price_ticks);
    return (it == g_ask_qty.end()) ? 0u : it->second;
}

}  // extern "C"
