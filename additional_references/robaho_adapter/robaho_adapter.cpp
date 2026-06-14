/*
 * robaho_adapter.cpp — robaho/cpp_orderbook behind the harness
 * matching_engine_api.h ABI.
 *
 * robaho's Exchange exposes a clean public API: buy/sell/cancel + an
 * ExchangeListener callback (onTrade, onOrder). The adapter:
 *   - registers an ExchangeListener that captures each Trade into a matcher-
 *     thread-owned queue, then drains it into harness Trade reports after
 *     submit.
 *   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject /
 *     ModifyReject above the engine, with a flat shadow vector
 *     (oid -> {exchangeId, price, side, remaining}) for id translation and
 *     the ack payload; the reject decision is the engine's return code
 *     wherever an exchangeId exists to ask with.
 *
 * No IOC type — residual cancelled by the adapter after submit.
 * Modify is cancel + reinsert (the engine has no native modify).
 *
 * Prices: workload ticks (canonical envelope 10,494–42,817) carried as
 * Fixed<7> integer values; ordering by tick is preserved bit-for-bit through
 * Fixed's internal fp comparator.
 */
#include "matching_engine_api.h"

#include "exchange.h"
#include "order.h"
#include "orderbook.h"
#include "fixed.h"

#include <cstdint>
#include <memory>
#include <string>
#include <algorithm>
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
};
/* harness oid -> shadow: the id TRANSLATION plus the ack payload. robaho
 * keys cancels on its own engine-assigned exchangeId (no client-id index),
 * returns a bare status int, and zeroes `remaining` before the cancel
 * callback fires — so xid/side/price/remaining must live here. The reject
 * DECISION belongs to the engine wherever it can be asked: for any recorded
 * xid, Exchange::cancel's return code adjudicates (0 = removed, -1 = not
 * resting; its OrderMap retains terminal orders, so a known xid never
 * throws). Only a harness id with no xid at all — never seen, or an IOC
 * (fully filled in-call, or its residual cancelled in-call; neither enters
 * the shadow) — is rejected without an engine call, because the engine's
 * API cannot express the question. Harness ids are dense and
 * 1-based, so a flat vector indexed by order_id holds this with no
 * per-insert allocation on the timed path. */
std::vector<Shadow> g_shadow;

/* Bounds-checked flat-vector lookup. */
inline Shadow* find_order(uint64_t ext_id) {
    return ext_id < g_shadow.size() ? &g_shadow[ext_id] : nullptr;
}

// Captured fills from the matching path (Listener::onTrade). Plain globals
// (not thread_local): the single matcher thread is the only caller, and TLS
// in a dlopen'd .so costs a __tls_get_addr call per touch.
struct CapturedFill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint64_t qty;
    int64_t  price;
};
std::vector<CapturedFill> g_fills;

inline F to_fixed(int64_t ticks) {
    // Fixed<7>(i, 0): fp = i * 10^7. The matching engine compares fp directly,
    // so a strictly increasing input mapping is preserved exactly.
    return F(ticks, 0);
}

inline int64_t from_fixed(F px) {
    // The fixed-point value's integer part — what we wrote in via to_fixed —
    // through the engine's own exact accessor (no float round-trip).
    return static_cast<int64_t>(px.intPart());
}

class HarnessListener : public ExchangeListener {
public:
    void onOrder(const Order&) override {}
    void onTrade(const Trade& tr) override {
        // Both ids arrive as the engine's string orderId — parse what the
        // engine reports (its Trade payload is the API for fill identity).
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
        if (Shadow* s = find_order(f.maker_id)) {
            if (s->remaining >= (uint32_t)f.qty)
                s->remaining -= (uint32_t)f.qty;
            else
                s->remaining = 0;
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
    /* resize (not reserve): zero-fills the slots (exchange_id == 0 = the
     * never-recorded sentinel) and faults the pages in, both untimed.
     * engine_prebuild grows past this if needed. */
    g_shadow.clear();
    g_shadow.resize(1u << 21);
    g_fills.reserve(64);
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
    g_exchange.reset();
    g_listener.reset();
    g_shadow.clear();
    g_fills.clear();
}

void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    g_fills.clear();   // defensive: provably empty — drain_fills clears after every submit
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
    g_shadow[o->order_id] = { xid, o->price_ticks, o->side, residual };
}

void engine_on_cancel(const cancel_t* c) {
    Shadow* sp = find_order(c->order_id);
    if (!sp || sp->exchange_id == 0) {
        // No exchangeId recorded — the engine cannot be asked (its cancel is
        // keyed by engine id and throws on an unknown one). Never-seen ids
        // and IOCs (fully filled or residual-cancelled in-call; never enter
        // the shadow) land here.
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    // The engine adjudicates: 0 = removed, -1 = not resting (already
    // cancelled / filled away).
    if (g_exchange->cancel(sp->exchange_id, SESSION) != 0) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             sp->side, sp->price, sp->remaining);
}

void engine_on_modify(const modify_t* m) {
    Shadow* sp = find_order(m->order_id);
    if (!sp || sp->exchange_id == 0) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint8_t side = sp->side;
    // The engine adjudicates the cancel half of cancel + reinsert.
    if (g_exchange->cancel(sp->exchange_id, SESSION) != 0) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    g_fills.clear();   // defensive: provably empty — drain_fills clears after every submit
    long xid = submit(side, m->order_id, m->new_price_ticks, m->new_quantity);
    const uint64_t filled = drain_fills(m->sequence_number);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    *sp = { xid, m->new_price_ticks, side, residual };
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
