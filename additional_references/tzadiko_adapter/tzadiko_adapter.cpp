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
 * Harness IOC maps onto the engine's own IOC type, FillAndKill: the
 * match-loop tail cancels any residual itself. As shipped that tail
 * self-deadlocks — it re-enters the locking public CancelOrder while
 * AddOrder still holds the book's non-recursive mutex — so build.sh patch 3
 * switches the two tail sites to the engine's own already-locked variant,
 * CancelOrderInternal (a correctness patch; trades and book state are what
 * an un-deadlocked tail would produce). Modify uses the engine's native
 * ModifyOrder (itself cancel + re-add, the harness contract). There are no
 * Ack/Reject report types, so the adapter synthesises OrderAck / CancelAck /
 * ModifyAck / CancelReject / ModifyReject above the engine and keeps a
 * per-order shadow (oid -> {price, side, remaining, alive}) to drive the
 * reject path and to echo side/price on CancelAck/ModifyAck.
 *
 * Trade format: each Tzadiko Trade carries a bid-side TradeInfo and an
 * ask-side TradeInfo. The maker is the side opposite the aggressor (whose
 * side we know from the harness call); the maker's price is the fill price
 * the harness records in ME_TRADE.price_ticks.
 *
 * Audit queries (best_bid / best_ask / depth_at) are answered from the
 * engine's own `Orderbook::GetOrderInfos()` snapshot. The snapshot walk is
 * O(N_resting) per call, but the harness excludes probe time from the timed
 * total, so the engine's native answer costs the measurement nothing.
 *
 * Note: Tzadiko's Orderbook spawns a "prune GoodForDay orders" background
 * thread on construction; it sleeps until 16:00 local time. The workload
 * contains no GoodForDay orders, so a wake (possible only if a run straddles
 * that boundary) finds nothing to cancel — it briefly takes the book mutex
 * and sleeps again. (See build.sh — the engine source uses Windows-only
 * `localtime_s`, which is patched to `localtime_r` for Linux.)
 */
#include "matching_engine_api.h"

#include "Orderbook.h"
#include "Order.h"
#include "OrderModify.h"
#include "OrderType.h"
#include "Side.h"
#include "Trade.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

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
/* Harness order ids are dense and 1-based, so a flat vector indexed by
 * order_id holds the shadow — no per-insert node allocation on the timed
 * path; a never-rested id is a zero slot (alive=false), the same reject
 * outcome a map miss produced. The shadow STATE itself is necessary: the
 * engine's CancelOrder returns void (silent on unknown ids), ModifyOrder
 * returns {} ambiguously, and no API exposes a resting order's fields.
 * No per-level depth shadow: the engine natively answers the audit queries
 * via GetOrderInfos(), and the harness excludes probe time from the timed
 * total — maintaining a parallel level map on every message bought nothing
 * the clock measures. */
std::vector<Shadow> g_shadow;

/* Bounds-checked flat-vector lookup; nullptr / alive=false = not resting. */
inline Shadow* find_order(uint64_t ext_id) {
    return ext_id < g_shadow.size() ? &g_shadow[ext_id] : nullptr;
}

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
        // Maintain the maker's per-order shadow (drives later rejects and
        // the CancelAck echo); the engine's own book carries level depth.
        if (Shadow* s = find_order(maker.orderId_)) {
            if (s->remaining >= maker.quantity_)
                s->remaining -= maker.quantity_;
            else
                s->remaining = 0;
            if (s->remaining == 0) s->alive = false;
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
    /* resize (not reserve): zero-fills the slots (alive=false) and faults the
     * pages in, both untimed. engine_prebuild grows past this if needed. */
    g_shadow.clear();
    g_shadow.resize(1u << 21);
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
    g_book.reset();   // joins the prune thread via Orderbook's destructor
    g_shadow.clear();
}

void engine_flush(void) {
    // Orderbook::AddOrder / CancelOrder / ModifyOrder are synchronous; nothing
    // in flight when control returns. The prune thread does not feed reports.
}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    // Engine-native order types: harness IOC is Tzadiko's FillAndKill. The
    // engine handles the residual itself — a no-cross FillAndKill returns
    // early without ever resting, and a partial fill is cancelled by the
    // MatchOrders tail (build.sh patch 3 fixes that tail's self-deadlock:
    // it called the locking CancelOrder under the already-held book mutex
    // instead of the engine's own CancelOrderInternal).
    const OrderType type = o->ioc ? OrderType::FillAndKill
                                  : OrderType::GoodTillCancel;
    auto order = std::make_shared<Order>(
        type,
        static_cast<OrderId>(o->order_id),
        (o->side == 0) ? Side::Buy : Side::Sell,
        static_cast<Price>(o->price_ticks),
        static_cast<Quantity>(o->quantity));

    Trades trades = g_book->AddOrder(std::move(order));
    const uint64_t filled = emit_trades(trades, o->sequence_number, o->side);
    const uint32_t residual = (filled < o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - filled)
                                  : 0u;

    if (o->ioc) {
        // The engine already dropped any residual (FillAndKill semantics);
        // synthesise the harness's CancelAck for the unfilled remainder.
        if (residual > 0) {
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        return;
    }

    // GTC. residual == 0 means the order fully filled on submit — record it
    // dead so a later cancel/modify rejects.
    g_shadow[o->order_id] = { o->price_ticks, o->side, residual, residual > 0 };
}

void engine_on_cancel(const cancel_t* c) {
    Shadow* s = find_order(c->order_id);
    if (!s || !s->alive) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    g_book->CancelOrder(static_cast<OrderId>(c->order_id));
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

    // Engine-native modify: Tzadiko's ModifyOrder is itself cancel + re-add
    // at the new price/qty (re-created with the order's remembered type —
    // GoodTillCancel here; IOC ids never enter the shadow), which is exactly
    // the harness contract. The shadow gate above stays as the reject
    // adjudicator only because the engine cannot adjudicate: ModifyOrder
    // returns {} for an unknown id, indistinguishable from a successful
    // non-crossing modify.
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);
    Trades trades = g_book->ModifyOrder(OrderModify{
        static_cast<OrderId>(m->order_id),
        (side == 0) ? Side::Buy : Side::Sell,
        static_cast<Price>(m->new_price_ticks),
        static_cast<Quantity>(m->new_quantity)});
    const uint64_t filled = emit_trades(trades, m->sequence_number, side);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    *s = { m->new_price_ticks, side, residual, residual > 0 };
}

// Audit queries are answered straight from the engine's native level
// snapshot, Orderbook::GetOrderInfos(). The snapshot walk is O(N_resting),
// but the harness excludes probe time from the timed total (192 query calls
// per run: 64 probe points x 3 queries) — so the engine's own answer costs
// the measurement nothing, where a parallel adapter-side level map cost
// sorted-map maintenance on every timed message.
int64_t engine_query_best_bid(void) {
    const auto infos = g_book->GetOrderInfos();
    const auto& bids = infos.GetBids();
    return bids.empty() ? INT64_MIN
                        : static_cast<int64_t>(bids.front().price_);
}

int64_t engine_query_best_ask(void) {
    const auto infos = g_book->GetOrderInfos();
    const auto& asks = infos.GetAsks();
    return asks.empty() ? INT64_MAX
                        : static_cast<int64_t>(asks.front().price_);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const auto infos = g_book->GetOrderInfos();
    const auto& levels = (side == 0) ? infos.GetBids() : infos.GetAsks();
    for (const auto& lvl : levels)
        if (static_cast<int64_t>(lvl.price_) == price_ticks)
            return static_cast<uint64_t>(lvl.quantity_);
    return 0;
}

}  // extern "C"
