/*
 * mansoor_adapter.cpp — mansoor-mamnoon/limit-order-book behind the harness
 * matching_engine_api.h ABI.
 *
 * mansoor's BookCore has native cancel + modify + IOC support and a callback-
 * style IEventLogger for fills. The adapter:
 *   - hooks the logger so each log_fill becomes a Trade report
 *   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject
 *     (the engine emits none of these to the harness's wire format)
 *   - maintains a shadow {oid -> {price, side, remaining, alive}} to drive the
 *     reject path and to echo side/price in CancelAck/ModifyAck.
 *
 * IOC: NewOrder.flags |= IOC delegated to the engine; the residual is detected
 * via ExecResult.filled and emitted as a CancelAck for the unfilled remainder.
 */
#include "matching_engine_api.h"

#include "lob/book_core.hpp"
#include "lob/price_levels.hpp"
#include "lob/logging.hpp"
#include "lob/types.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <limits>
#include <memory>

namespace {

using namespace lob;

// 16-bit price index space is ample for the workload's 26,920–64,843 range and
// keeps the PriceLevelsContig footprint reasonable. Configure with margin.
constexpr Tick MIN_TICK  = 0;
constexpr Tick MAX_TICK  = 100000;
constexpr Tick TICK_SIZE = 1;

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

std::unique_ptr<PriceLevelsContig> g_bids;
std::unique_ptr<PriceLevelsContig> g_asks;
std::unique_ptr<BookCore>        g_book;

struct Shadow {
    int64_t  price;
    uint8_t  side;       // 0 = buy, 1 = sell
    uint32_t remaining;
    bool     alive;
};
std::unordered_map<uint64_t, Shadow> g_shadow;

// Per-call context: the harness message's seq, propagated into the engine's
// log_fill callback so Trade reports carry the originating message's seq.
uint64_t g_cur_seq    = 0;
uint64_t g_cur_taker  = 0;
uint8_t  g_cur_takerSide = 0;

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

// Custom IEventLogger that converts mansoor's log_fill calls into harness Trade
// reports. The other log_* hooks are not needed — the adapter synthesises
// OrderAck / CancelAck / ModifyAck above the engine.
class HarnessLogger final : public IEventLogger {
public:
    void log_new(const NewOrder&, bool, Tick, Timestamp) override {}
    void log_cancel(OrderId, Timestamp) override {}
    void log_fill(Tick px, Quantity qty, Side /*liq_side*/,
                  OrderId passive_id, OrderId taker_id, Timestamp /*ts*/) override {
        me_report_t r{};
        r.type            = ME_TRADE;
        r.sequence_number = g_cur_seq;
        r.price_ticks     = static_cast<int64_t>(px);
        r.quantity        = static_cast<uint32_t>(qty);
        r.maker_order_id  = static_cast<uint64_t>(passive_id);
        r.taker_order_id  = static_cast<uint64_t>(taker_id);
        emit(r);
        if (auto it = g_shadow.find(passive_id); it != g_shadow.end()) {
            if (it->second.remaining >= (uint32_t)qty)
                it->second.remaining -= (uint32_t)qty;
            else
                it->second.remaining = 0;
            if (it->second.remaining == 0) it->second.alive = false;
        }
    }
};

std::unique_ptr<HarnessLogger> g_logger;

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* sink) {
    g_transport = transport;
    g_sink      = sink;
    PriceBand band{ MIN_TICK, MAX_TICK, TICK_SIZE };
    g_bids   = std::make_unique<PriceLevelsContig>(band);
    g_asks   = std::make_unique<PriceLevelsContig>(band);
    g_logger = std::make_unique<HarnessLogger>();
    g_book   = std::make_unique<BookCore>(*g_bids, *g_asks, g_logger.get());
    g_shadow.clear();
    g_shadow.reserve(1u << 21);
}

void engine_shutdown(void) {
    g_book.reset();
    g_logger.reset();
    g_asks.reset();
    g_bids.reset();
    g_shadow.clear();
}

void engine_flush(void) { /* synchronous matcher, no in-flight work */ }

void engine_on_new_order(const new_order_t* o) {
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    g_cur_seq       = o->sequence_number;
    g_cur_taker     = o->order_id;
    g_cur_takerSide = o->side;

    NewOrder no{};
    no.seq   = o->sequence_number;
    no.ts    = 0;
    no.id    = o->order_id;
    no.user  = o->order_id;
    no.side  = (o->side == 0) ? Side::Bid : Side::Ask;
    no.price = static_cast<Tick>(o->price_ticks);
    no.qty   = static_cast<Quantity>(o->quantity);
    no.flags = (o->ioc ? IOC : NONE);

    ExecResult r = g_book->submit_limit(no);
    const uint32_t residual = (r.filled < (Quantity)o->quantity)
                                  ? static_cast<uint32_t>(o->quantity - r.filled)
                                  : 0u;

    if (o->ioc) {
        if (residual > 0)
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        return;
    }

    // GTC: shadow tracks the resting remainder (zero => already fully filled,
    // alive=false so a subsequent cancel/modify will reject).
    g_shadow[o->order_id] = { o->price_ticks, o->side, residual, residual > 0 };
}

void engine_on_cancel(const cancel_t* c) {
    auto it = g_shadow.find(c->order_id);
    if (it == g_shadow.end() || !it->second.alive) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    const Shadow s = it->second;
    bool ok = g_book->cancel(c->order_id);
    if (!ok) {
        // Shadow disagreed with the engine — treat as not-resting and reject.
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

    // The harness's modify contract is cancel + reinsert (queue-priority lost),
    // not the in-place-resize that mansoor's modify() implements. Do the cancel
    // explicitly here and re-submit as a new order so the matching path is the
    // same as any other crossing NEW — and the resulting trades carry the
    // modify's seq.
    if (!g_book->cancel(m->order_id)) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        it->second.alive = false;
        return;
    }
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             side, m->new_price_ticks, m->new_quantity);

    g_cur_seq       = m->sequence_number;
    g_cur_taker     = m->order_id;
    g_cur_takerSide = side;

    NewOrder no{};
    no.seq   = m->sequence_number;
    no.ts    = 0;
    no.id    = m->order_id;
    no.user  = m->order_id;
    no.side  = (side == 0) ? Side::Bid : Side::Ask;
    no.price = static_cast<Tick>(m->new_price_ticks);
    no.qty   = static_cast<Quantity>(m->new_quantity);
    no.flags = NONE;
    ExecResult r = g_book->submit_limit(no);
    const uint32_t residual = (r.filled < (Quantity)m->new_quantity)
                                  ? static_cast<uint32_t>(m->new_quantity - r.filled)
                                  : 0u;
    it->second = { m->new_price_ticks, side, residual, residual > 0 };
}

int64_t engine_query_best_bid(void) {
    Tick t = g_bids->best_bid();
    return (t == std::numeric_limits<Tick>::min()) ? INT64_MIN : static_cast<int64_t>(t);
}

int64_t engine_query_best_ask(void) {
    Tick t = g_asks->best_ask();
    return (t == std::numeric_limits<Tick>::max()) ? INT64_MAX : static_cast<int64_t>(t);
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    if (price_ticks < MIN_TICK || price_ticks > MAX_TICK) return 0;
    auto& book = (side == 0) ? *g_bids : *g_asks;
    if (!book.has_level(static_cast<Tick>(price_ticks))) return 0;
    return static_cast<uint64_t>(book.get_level(static_cast<Tick>(price_ticks)).total_qty);
}

}  // extern "C"
