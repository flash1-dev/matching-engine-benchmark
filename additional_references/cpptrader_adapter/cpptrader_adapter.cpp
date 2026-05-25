/*
 * cpptrader_adapter.cpp — chronoxor/CppTrader's MarketManager behind the
 * harness matching_engine_api.h ABI.
 *
 * CppTrader's MarketManager handles native limit/IOC/FOK/AON with its own
 * MarketHandler callback interface (onAddOrder, onUpdateOrder, onDeleteOrder,
 * onExecuteOrder). The adapter:
 *   - Installs a MarketHandler subclass and captures fills via onExecuteOrder.
 *     CppTrader's matching loop calls onExecuteOrder twice per fill — first
 *     for the maker (resting) order, then for the taker (incoming) — so the
 *     handler pairs consecutive callbacks into one Trade report.
 *   - Synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
 *     ModifyReject above the engine (the engine's MarketHandler callbacks
 *     don't map onto the harness's wire format).
 *   - Tracks a shadow map (oid -> {side, price, alive}) for the reject path
 *     and to echo side/price on CancelAck/ModifyAck. The resting quantity on
 *     CancelAck comes from g_manager->GetOrder() to stay in sync after fills.
 *
 * IOC: CppTrader has native IOC. AddOrder with TIF=IOC matches what it can
 * and discards the rest without resting — no per-fill flag needed; the
 * residual is the input qty minus the qty the taker received in fills.
 *
 * MODIFY: the engine's ModifyOrder already implements cancel-and-rematch (it
 * pulls the order off the book, updates price/qty, runs MatchLimit, and
 * re-adds whatever leftover remains). That matches the harness's "cancel +
 * reinsert at new price/qty, losing queue priority" contract, and crossing
 * fills surface through the same onExecuteOrder path so they pick up the
 * modify message's seq via the adapter's per-call context.
 */
#include "matching_engine_api.h"

#include "trader/matching/market_manager.h"
#include "trader/matching/market_handler.h"
#include "trader/matching/order.h"
#include "trader/matching/order_book.h"
#include "trader/matching/symbol.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace {

using namespace CppTrader::Matching;

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

constexpr uint32_t SYMBOL_ID = 1;

struct Shadow {
    int64_t price;   // last known resting price in ticks
    uint8_t side;    // 0 = buy, 1 = sell
    bool    alive;
};
std::unordered_map<uint64_t, Shadow> g_shadow;

// Per-call context. Set immediately before the engine call that may produce
// fills; read by the MarketHandler's onExecuteOrder to attribute every Trade
// to the aggressor's message seq, and to tally how much the taker has filled.
uint64_t g_cur_seq      = 0;
uint64_t g_cur_taker    = 0;
uint64_t g_taker_filled = 0;   // running sum of qty filled by the current taker

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

// CppTrader's MatchOrder calls onExecuteOrder(maker, price, qty) FIRST then
// onExecuteOrder(taker, price, qty) per fill. The handler pairs consecutive
// callbacks (maker, then taker) into one harness Trade report. For our
// workload (only GTC and IOC) every fill follows that pattern; FOK/AON have a
// different single-callback shape but the workload never produces them.
class HarnessHandler : public MarketHandler {
public:
    void onExecuteOrder(const Order& order, uint64_t price, uint64_t qty) override {
        if (!have_maker_) {
            pending_maker_id_ = order.Id;
            pending_price_    = price;
            pending_qty_      = qty;
            have_maker_       = true;
            return;
        }
        // Second callback in the pair: the taker (aggressor).
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = g_cur_seq;
        r.price_ticks     = static_cast<int64_t>(pending_price_);
        r.quantity        = static_cast<uint32_t>(pending_qty_);
        r.maker_order_id  = pending_maker_id_;
        r.taker_order_id  = order.Id;
        emit(r);
        g_taker_filled += pending_qty_;
        // Maker liveness is re-derived from g_manager->GetOrder() at the
        // shadow access sites — the engine is the source of truth.
        have_maker_ = false;
    }

    void reset_pairing() noexcept {
        have_maker_       = false;
        pending_maker_id_ = 0;
        pending_price_    = 0;
        pending_qty_      = 0;
    }

    // The other MarketHandler hooks are intentionally no-ops; the adapter
    // synthesises all Ack/Reject reports above the engine.

private:
    bool     have_maker_       = false;
    uint64_t pending_maker_id_ = 0;
    uint64_t pending_price_    = 0;
    uint64_t pending_qty_      = 0;
};

std::unique_ptr<HarnessHandler> g_handler;
std::unique_ptr<MarketManager>  g_manager;
const OrderBook*                g_book = nullptr;

inline void begin_call(uint64_t seq, uint64_t taker_id) {
    g_cur_seq      = seq;
    g_cur_taker    = taker_id;
    g_taker_filled = 0;
    g_handler->reset_pairing();
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_handler   = std::make_unique<HarnessHandler>();
    g_manager   = std::make_unique<MarketManager>(*g_handler);
    g_shadow.clear();
    g_shadow.reserve(1u << 21);

    // Symbol::Symbol expects char[8] and memcpys 8 bytes — pad to 8.
    const char name[8] = { 'W', 'O', 'R', 'K', 'L', 'D', 0, 0 };
    Symbol sym(SYMBOL_ID, name);
    g_manager->AddSymbol(sym);
    g_manager->AddOrderBook(sym);
    g_manager->EnableMatching();
    g_book = g_manager->GetOrderBook(SYMBOL_ID);
}

void engine_shutdown(void) {
    g_book = nullptr;
    g_manager.reset();
    g_handler.reset();
    g_shadow.clear();
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    begin_call(o->sequence_number, o->order_id);

    const OrderTimeInForce tif = o->ioc ? OrderTimeInForce::IOC : OrderTimeInForce::GTC;
    Order order = (o->side == 0)
        ? Order::BuyLimit(o->order_id, SYMBOL_ID,
                          static_cast<uint64_t>(o->price_ticks),
                          static_cast<uint64_t>(o->quantity), tif)
        : Order::SellLimit(o->order_id, SYMBOL_ID,
                           static_cast<uint64_t>(o->price_ticks),
                           static_cast<uint64_t>(o->quantity), tif);

    g_manager->AddOrder(order);

    if (o->ioc) {
        // Native IOC: engine never inserts the residual into the book.
        const uint64_t filled = g_taker_filled;
        if (filled < o->quantity) {
            const uint32_t residual = static_cast<uint32_t>(o->quantity - filled);
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        return;
    }

    // GTC: the engine may have rested the order, or fully filled it on the
    // way in. Track via shadow so a later cancel/modify of a fully-filled
    // order rejects (matching the harness "not resting" contract).
    const Order* resting = g_manager->GetOrder(o->order_id);
    const bool alive = (resting != nullptr) && (resting->LeavesQuantity > 0);
    g_shadow[o->order_id] = { o->price_ticks, o->side, alive };
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_shadow.find(c->order_id);
    const Order* live = g_manager->GetOrder(c->order_id);
    if (it == g_shadow.end() || !it->second.alive || live == nullptr) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        if (it != g_shadow.end()) it->second.alive = false;
        return;
    }
    const uint8_t  side  = it->second.side;
    const int64_t  price = static_cast<int64_t>(live->Price);
    const uint32_t qty   = static_cast<uint32_t>(live->LeavesQuantity);
    g_manager->DeleteOrder(c->order_id);
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id, side, price, qty);
    it->second.alive = false;
}

void engine_on_modify(const modify_t* m) {
    auto it = g_shadow.find(m->order_id);
    if (it == g_shadow.end() || !it->second.alive
        || g_manager->GetOrder(m->order_id) == nullptr) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        if (it != g_shadow.end()) it->second.alive = false;
        return;
    }
    const uint8_t side = it->second.side;

    // Harness contract: ModifyAck precedes crossing trades from the reinsert.
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    begin_call(m->sequence_number, m->order_id);

    g_manager->ModifyOrder(m->order_id,
                           static_cast<uint64_t>(m->new_price_ticks),
                           static_cast<uint64_t>(m->new_quantity));

    // Refresh the shadow with the post-modify resting state. The engine
    // (under ModifyOrder + MatchLimit) may have re-rested the order, fully
    // consumed it, or left a residual. Truth is whatever the engine holds.
    const Order* resting = g_manager->GetOrder(m->order_id);
    const bool alive = (resting != nullptr) && (resting->LeavesQuantity > 0);
    it->second = { m->new_price_ticks, side, alive };
}

int64_t engine_query_best_bid(void) {
    const LevelNode* lvl = g_book->best_bid();
    return (lvl == nullptr) ? INT64_MIN : static_cast<int64_t>(lvl->Price);
}

int64_t engine_query_best_ask(void) {
    const LevelNode* lvl = g_book->best_ask();
    return (lvl == nullptr) ? INT64_MAX : static_cast<int64_t>(lvl->Price);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    if (price_ticks < 0) return 0;
    const uint64_t price = static_cast<uint64_t>(price_ticks);
    const LevelNode* lvl = (side == 0) ? g_book->GetBid(price) : g_book->GetAsk(price);
    return (lvl == nullptr) ? 0ull : static_cast<uint64_t>(lvl->TotalVolume);
}

}  // extern "C"
