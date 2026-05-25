/*
 * robaho_adapter.cpp — robaho/cpp_orderbook behind the harness matching_engine_
 * api.h ABI.
 *
 * robaho's Exchange exposes a clean public API: buy/sell/cancel + an
 * ExchangeListener callback (onTrade, onOrder). The adapter:
 *   - registers an ExchangeListener that captures each Trade into a thread-
 *     local queue, then drains it into harness Trade reports after submit.
 *   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
 *     ModifyReject above the engine. Shadow map (oid -> {exchangeId, price,
 *     side, remaining, alive}).
 *
 * No IOC type — residual cancelled by the adapter after submit.
 * Modify is cancel + reinsert (the engine has no native modify).
 *
 * Prices: workload ticks (int64_t in [26920, 64843]) carried as Fixed<7>
 * integer values; ordering by tick is preserved bit-for-bit through Fixed's
 * internal fp comparator.
 */
#include "matching_engine_api.h"

#include "exchange.h"
#include "order.h"
#include "orderbook.h"
#include "fixed.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

constexpr const char* INSTRUMENT = "SYM";
constexpr const char* SESSION    = "s";

struct Shadow {
    long     exchange_id;
    int64_t  price;        // workload ticks
    uint8_t  side;         // 0 buy, 1 sell
    uint32_t remaining;
    bool     alive;
};
std::unordered_map<uint64_t, Shadow> g_shadow;

// Captured fills from the matching path (Listener::onTrade).
struct CapturedFill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint64_t qty;
    int64_t  price;
};
thread_local std::vector<CapturedFill> g_fills;
thread_local uint64_t g_cur_seq    = 0;
thread_local uint64_t g_cur_taker  = 0;

inline F to_fixed(int64_t ticks) {
    // Fixed<7>(i, 0): fp = i * 10^7. The matching engine compares fp directly,
    // so a strictly increasing input mapping is preserved exactly.
    return F(static_cast<int64_t>(ticks), 0);
}

inline int64_t from_fixed(F px) {
    // The fixed-point value's "integer" part — what we wrote in via to_fixed.
    return static_cast<int64_t>(static_cast<double>(px));
}

class HarnessListener : public ExchangeListener {
public:
    void onOrder(const Order&) override {}
    void onTrade(const Trade& tr) override {
        const uint64_t maker = static_cast<uint64_t>(std::stoull(tr.opposite.orderId()));
        const uint64_t taker = static_cast<uint64_t>(std::stoull(tr.aggressor.orderId()));
        g_fills.push_back({ maker, taker,
                            static_cast<uint64_t>(tr.quantity),
                            from_fixed(tr.price) });
    }
};

std::unique_ptr<HarnessListener> g_listener;
std::unique_ptr<Exchange>        g_exchange;

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

inline long submit(uint8_t side, uint64_t oid, int64_t price_ticks, uint32_t qty) {
    const std::string oid_str = std::to_string(oid);
    return (side == 0)
        ? g_exchange->buy (SESSION, INSTRUMENT, to_fixed(price_ticks), (int)qty, oid_str)
        : g_exchange->sell(SESSION, INSTRUMENT, to_fixed(price_ticks), (int)qty, oid_str);
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    g_listener  = std::make_unique<HarnessListener>();
    g_exchange  = std::make_unique<Exchange>(*g_listener);
    g_shadow.clear();
    g_shadow.reserve(1u << 21);
    g_fills.reserve(64);
}

void engine_shutdown(void) {
    g_exchange.reset();
    g_listener.reset();
    g_shadow.clear();
    g_fills.clear();
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    g_cur_seq   = o->sequence_number;
    g_cur_taker = o->order_id;
    g_fills.clear();
    long xid = submit(o->side, o->order_id, o->price_ticks, o->quantity);
    const uint64_t filled = drain_fills(o->sequence_number);
    const uint32_t residual = (filled < o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - filled)
                                  : 0u;

    if (o->ioc) {
        if (residual > 0) {
            g_exchange->cancel(xid, SESSION);
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        }
        return;
    }
    g_shadow[o->order_id] = { xid, o->price_ticks, o->side, residual, residual > 0 };
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_shadow.find(c->order_id);
    if (it == g_shadow.end() || !it->second.alive) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    const Shadow s = it->second;
    int rc = g_exchange->cancel(s.exchange_id, SESSION);
    if (rc != 0) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        it->second.alive = false;
        return;
    }
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
    const uint8_t side = it->second.side;
    const long old_xid = it->second.exchange_id;
    int rc = g_exchange->cancel(old_xid, SESSION);
    if (rc != 0) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        it->second.alive = false;
        return;
    }
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    g_cur_seq   = m->sequence_number;
    g_cur_taker = m->order_id;
    g_fills.clear();
    long xid = submit(side, m->order_id, m->new_price_ticks, m->new_quantity);
    const uint64_t filled = drain_fills(m->sequence_number);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    it->second = { xid, m->new_price_ticks, side, residual, residual > 0 };
}

int64_t engine_query_best_bid(void) {
    const Book b = g_exchange->book(INSTRUMENT);
    if (b.bids.empty()) return INT64_MIN;
    return from_fixed(b.bids.front().price);
}

int64_t engine_query_best_ask(void) {
    const Book b = g_exchange->book(INSTRUMENT);
    if (b.asks.empty()) return INT64_MAX;
    return from_fixed(b.asks.front().price);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const Book b = g_exchange->book(INSTRUMENT);
    const auto& levels = (side == 0) ? b.bids : b.asks;
    for (const auto& lvl : levels) {
        if (from_fixed(lvl.price) == price_ticks)
            return static_cast<uint64_t>(lvl.quantity);
    }
    return 0;
}

}  // extern "C"
