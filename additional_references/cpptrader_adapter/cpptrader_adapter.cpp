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
 *   - Gates every cancel/modify on g_manager->GetOrder() — the engine's own
 *     id index — which also supplies the side/price/qty echoed on CancelAck
 *     and the side on ModifyAck; GetOrder == nullptr drives CancelReject /
 *     ModifyReject. The adapter keeps no order state of its own.
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

namespace {

using namespace CppTrader::Matching;

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

constexpr uint32_t SYMBOL_ID = 1;

/* No adapter-side order state: CppTrader's MarketManager keys every resting
 * order by id in its own open-addressing hash (pool-allocated nodes), and
 * GetOrder(id) returns nullptr exactly when an order is not resting (the
 * engine never keeps zero-leaves orders — fully-filled-on-entry is never
 * inserted, filled makers / cancels / consumed modifies are erased, IOC never
 * rests). Cancel/modify decisions and ack field echo all come from GetOrder. */

// Per-call context. Set immediately before the engine call that may produce
// fills; read by the MarketHandler's onExecuteOrder to attribute every Trade
// to the aggressor's message seq, and to tally how much the taker has filled.
// (The taker id itself arrives in the engine's callback.)
uint64_t g_cur_seq      = 0;
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
        // Maker liveness lives in the engine's own id index (GetOrder) — the
        // engine is the source of truth.
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

inline void begin_call(uint64_t seq) {
    g_cur_seq      = seq;
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
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    begin_call(o->sequence_number);

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

    // GTC: the engine either rested the order in its own id index or fully
    // filled it on the way in (in which case it was never inserted). A later
    // cancel/modify asks GetOrder directly — nothing to record here.
}

void engine_on_cancel(const cancel_t* c) {
    // The engine's own id index answers the resting test and supplies the
    // ack's side/price/qty. Capture them BEFORE DeleteOrder — the node is
    // pool-released inside it.
    const Order* live = g_manager->GetOrder(c->order_id);
    if (live == nullptr) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    const uint8_t  side  = static_cast<uint8_t>(live->Side);
    const int64_t  price = static_cast<int64_t>(live->Price);
    const uint32_t qty   = static_cast<uint32_t>(live->LeavesQuantity);
    g_manager->DeleteOrder(c->order_id);
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id, side, price, qty);
}

void engine_on_modify(const modify_t* m) {
    // One GetOrder answers the resting test and supplies the ack's side; the
    // engine's ModifyOrder then does its own (unavoidable) internal lookup.
    const Order* live = g_manager->GetOrder(m->order_id);
    if (live == nullptr) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint8_t side = static_cast<uint8_t>(live->Side);

    // Emit the ModifyAck before the engine call (the side is already in
    // hand); the canonical stream stable-sorts by (seq, type), so ack/trade
    // emission order within one message never affects the hash.
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    begin_call(m->sequence_number);

    // CppTrader's ModifyOrder natively implements the harness modify contract
    // (delete + reprice + rematch + re-add); post-modify resting state lives
    // in the engine's index, so there is nothing to refresh.
    g_manager->ModifyOrder(m->order_id,
                           static_cast<uint64_t>(m->new_price_ticks),
                           static_cast<uint64_t>(m->new_quantity));
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
