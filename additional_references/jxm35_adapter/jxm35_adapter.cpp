/*
 * jxm35_adapter.cpp — jxm35/LimitOrderBook-MatchingEngine behind the harness
 * matching_engine_api.h ABI.
 *
 * The engine's OrderBook<MDPublisher> matches internally during AddOrder, but
 * its MarketDataPublisher API has no per-fill callback that exposes the
 * maker/taker order ids (notify_trade exists but isn't called from TryMatch).
 * For byte-identical Trade reports the adapter needs that information.
 *
 * The adapter compiles a build-time-patched OrderBook.cpp (see the patch
 * script in build.sh) that adds a single hook call inside the matching loop.
 * The hook records each fill into a plain-global vector (single matcher
 * thread; see the note at g_fills) which the adapter drains into Trade
 * reports after AddOrder / AmendOrder returns. No other engine source is
 * modified.
 *
 * The engine has no IOC / FOK / POST-ONLY flags and no native ack/reject
 * reports, so the adapter synthesises them above the engine: rejects are
 * adjudicated by the engine's own ContainsOrder, and a flat shadow vector
 * (order_id -> price/side/remaining) echoes the ack payload no engine API
 * returns. Modify uses the engine's native AmendOrder (its own remove +
 * re-add at the new price/qty, queue priority lost); the hook captures the
 * re-add's crossing trades and the adapter stamps them with the modify's
 * seq when draining.
 */
#include "matching_engine_api.h"

// Bring jxm35's headers in. The engine is templated on MDPublisher, so we
// must use one of the two publishers the engine itself explicitly
// instantiates.
#include "core/OrderBook.h"
#include "publisher/MarketDataPublisher.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// Captured fills from the matching hot path. The patched TryMatch in
// OrderBook.cpp appends to this vector via the hook below. A plain global
// (not thread_local): the single matcher thread is the only caller, and a
// thread_local in a dlopen'd .so would cost a __tls_get_addr call per fill.
struct CapturedFill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint64_t qty;
    int64_t  price;
};
std::vector<CapturedFill> g_fills;

struct Shadow {
    int64_t  price;
    uint8_t  side;
    uint32_t remaining;
};
/* PAYLOAD ECHO ONLY. The reject decision belongs to the engine — its public
 * ContainsOrder is the existence API (RemoveOrder signals a missing id only
 * by throwing) — while this shadow supplies the side/price/remaining that no
 * engine API returns for a cancelled order. Harness order ids are dense and
 * 1-based (a permutation of 1..N_new), so a flat vector indexed by order_id
 * holds it with no per-insert allocation on the timed path. */
std::vector<Shadow> g_shadow;

/* Bounds-checked flat-vector lookup. */
inline Shadow* find_order(uint64_t ext_id) {
    return ext_id < g_shadow.size() ? &g_shadow[ext_id] : nullptr;
}

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

}  // namespace

// The hook invoked by the patched TryMatch (see the patch script in build.sh).
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
    /* resize (not reserve): value-initialises every slot (all fields zero)
     * and faults the pages in, both outside the timed window. engine_prebuild
     * grows past this if a workload ever uses ids beyond 2M. */
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

    g_fills.clear();   // defensive: provably empty — drain_fills clears after every engine call
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
    g_shadow[o->order_id] = { o->price_ticks, o->side, residual };
}

void engine_on_cancel(const cancel_t* c) {
    // The engine adjudicates: ContainsOrder is its public existence API
    // (RemoveOrder signals a missing id only by throwing). The shadow only
    // supplies the ack payload the engine cannot return.
    if (!g_book->ContainsOrder(static_cast<long>(c->order_id))) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    Shadow* s = find_order(c->order_id);
    if (!s) {   // defensive: unreachable — every engine-resting id has a slot
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    g_book->RemoveOrder(static_cast<long>(c->order_id));
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,
             s->side, s->price, s->remaining);
}

void engine_on_modify(const modify_t* m) {
    if (!g_book->ContainsOrder(static_cast<long>(m->order_id))) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    Shadow* s = find_order(m->order_id);
    if (!s) {   // defensive: unreachable — every engine-resting id has a slot
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    const uint8_t side = s->side;

    // Engine-native modify: AmendOrder is the engine's own remove + re-add
    // at the new price/qty — exactly the harness contract (queue priority
    // lost), with crossing trades captured by the hook inside its AddOrder
    // half and stamped with the modify's seq.
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    OrderCore core(static_cast<long>(m->order_id), std::string("u"), static_cast<int>(INSTRUMENT_ID));
    Order order(core, static_cast<long>(m->new_price_ticks),
                static_cast<uint32_t>(m->new_quantity), side == 0);
    g_fills.clear();   // defensive: provably empty — drain_fills clears after every engine call
    g_book->AmendOrder(static_cast<long>(m->order_id), order);
    const uint64_t filled = drain_fills(m->sequence_number);
    const uint32_t residual = (filled < m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - filled)
                                  : 0u;
    *s = { m->new_price_ticks, side, residual };
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
