/*
 * jxm35_adapter.cpp — jxm35/LimitOrderBook-MatchingEngine behind the harness
 * matching_engine_api.h ABI.
 *
 * The engine's OrderBook<MDPublisher> matches internally during AddOrder, but
 * its MarketDataPublisher API has no per-fill callback that exposes the
 * maker/taker order ids (notify_trade exists but isn't called from TryMatch).
 * For byte-identical Trade reports the adapter needs that information.
 *
 * The adapter compiles a sed-patched copy of OrderBook.cpp (see build.sh) that
 * adds a single hook call inside the matching loop. The hook records each fill
 * into a thread-local vector which the adapter drains into Trade reports after
 * AddOrder / AmendOrder returns. No other engine source is modified.
 *
 * The engine has no IOC / FOK / POST-ONLY flags and no native ack/reject
 * reports, so the adapter synthesises all of them above the engine with a
 * shadow map (oid -> price, side, remaining qty, alive). Modify is handled as
 * cancel + re-add at the new price (jxm35::AmendOrder does the same internally
 * but doesn't carry the message-seq through; we do it explicitly so the trades
 * emitted by the re-add carry the modify's seq).
 */
#include "matching_engine_api.h"

// Bring jxm35's headers in. The engine is templated on MDPublisher so we have
// to instantiate it explicitly with one of its provided publishers.
#include "core/OrderBook.h"
#include "publisher/MarketDataPublisher.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// Captured fills from the matching hot path. The patched TryMatch in
// OrderBook.cpp appends to this vector via the hook below.
struct CapturedFill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint64_t qty;
    int64_t  price;
};
thread_local std::vector<CapturedFill> g_fills;

struct Shadow {
    int64_t  price;
    uint8_t  side;
    uint32_t remaining;
    bool     alive;
};
std::unordered_map<uint64_t, Shadow> g_shadow;

constexpr uint32_t INSTRUMENT_ID = 1;

// jxm35 explicitly instantiates OrderBook<> only for two publishers
// (NullMarketDataPublisher and MarketDataPublisher). Use the null one — the
// publish callbacks don't matter for our purposes (we capture trades via the
// hook in the patched TryMatch), and the null path has zero per-call cost.
using Publisher = mdfeed::NullMarketDataPublisher;
std::unique_ptr<Publisher>            g_publisher;
std::unique_ptr<OrderBook<Publisher>> g_book;

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

inline uint64_t drain_fills(uint64_t taker_seq) {
    uint64_t taker_filled = 0;
    for (const auto& f : g_fills) {
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = taker_seq;
        r.price_ticks     = f.price;
        r.quantity        = static_cast<uint32_t>(f.qty);
        r.maker_order_id  = f.maker_id;
        r.taker_order_id  = f.taker_id;
        emit(r);
        taker_filled += f.qty;
        if (auto it = g_shadow.find(f.maker_id); it != g_shadow.end()) {
            if (it->second.remaining >= (uint32_t)f.qty)
                it->second.remaining -= (uint32_t)f.qty;
            else
                it->second.remaining = 0;
            if (it->second.remaining == 0) it->second.alive = false;
        }
    }
    g_fills.clear();
    return taker_filled;
}

}  // namespace

// The hook invoked by the patched TryMatch (see build.sh's sed patch).
extern "C" void __jxm35_adapter_trade_hook(uint64_t maker_id, uint64_t taker_id,
                                           uint64_t qty, int64_t price) {
    g_fills.push_back({ maker_id, taker_id, qty, price });
}

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_publisher = std::make_unique<Publisher>();
    mdfeed::MDAdapter<Publisher> adapter(INSTRUMENT_ID, *g_publisher);
    Security security{ "SYM", "SYM", static_cast<int>(INSTRUMENT_ID) };
    g_book      = std::make_unique<OrderBook<Publisher>>(security, adapter);
    g_shadow.clear();
    g_shadow.reserve(1u << 21);
    g_fills.reserve(64);
}

void engine_shutdown(void) {
    g_book.reset();
    g_publisher.reset();
    g_shadow.clear();
    g_fills.clear();
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    OrderCore core(static_cast<long>(o->order_id), std::string("u"), static_cast<int>(INSTRUMENT_ID));
    Order order(core, static_cast<long>(o->price_ticks),
                static_cast<uint32_t>(o->quantity), o->side == 0);

    g_fills.clear();
    g_book->AddOrder(order);
    const uint64_t filled = drain_fills(o->sequence_number);
    const uint32_t residual = (filled < o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - filled)
                                  : 0u;

    if (o->ioc) {
        if (residual > 0) {
            // jxm35 has no IOC type — the residual rested. Cancel it.
            g_book->RemoveOrder(static_cast<long>(o->order_id));
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        return;
    }
    g_shadow[o->order_id] = { o->price_ticks, o->side, residual, residual > 0 };
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_shadow.find(c->order_id);
    if (it == g_shadow.end() || !it->second.alive
        || !g_book->ContainsOrder(static_cast<long>(c->order_id))) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        if (it != g_shadow.end()) it->second.alive = false;
        return;
    }
    g_book->RemoveOrder(static_cast<long>(c->order_id));
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             it->second.side, it->second.price, it->second.remaining);
    it->second.alive = false;
}

void engine_on_modify(const modify_t* m) {
    auto it = g_shadow.find(m->order_id);
    if (it == g_shadow.end() || !it->second.alive
        || !g_book->ContainsOrder(static_cast<long>(m->order_id))) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        if (it != g_shadow.end()) it->second.alive = false;
        return;
    }
    const uint8_t side = it->second.side;

    // Harness contract: cancel + reinsert at the new price/qty, with the
    // crossing trades carrying the modify message's seq.
    g_book->RemoveOrder(static_cast<long>(m->order_id));
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    OrderCore core(static_cast<long>(m->order_id), std::string("u"), static_cast<int>(INSTRUMENT_ID));
    Order order(core, static_cast<long>(m->new_price_ticks),
                static_cast<uint32_t>(m->new_quantity), side == 0);
    g_fills.clear();
    g_book->AddOrder(order);
    const uint64_t filled = drain_fills(m->sequence_number);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    it->second = { m->new_price_ticks, side, residual, residual > 0 };
}

int64_t engine_query_best_bid(void) {
    auto v = g_book->GetBestBidPrice();
    return v.has_value() ? static_cast<int64_t>(*v) : INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    auto v = g_book->GetBestAskPrice();
    return v.has_value() ? static_cast<int64_t>(*v) : INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const auto quantities = (side == 0)
        ? g_book->GetBidQuantities()
        : g_book->GetAskQuantities();
    auto it = quantities.find(static_cast<long>(price_ticks));
    return (it == quantities.end()) ? 0 : static_cast<uint64_t>(it->second);
}

}  // extern "C"
