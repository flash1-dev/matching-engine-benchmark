/*
 * mansoor_adapter.cpp — mansoor-mamnoon/limit-order-book behind the harness
 * matching_engine_api.h ABI.
 *
 * mansoor's BookCore has native cancel + modify + IOC support and a callback-
 * style IEventLogger for fills. The adapter:
 *   - hooks the logger so each log_fill becomes a Trade report
 *   - synthesises OrderAck / CancelAck / ModifyAck / CancelReject / ModifyReject
 *     (the engine emits none of these to the harness's wire format)
 *   - keeps a flat payload-echo vector {oid -> {price, side}} for the
 *     CancelAck only; the reject decision is the engine's own (cancel()'s
 *     bool, modify()'s {0,0} not-found result).
 *
 * IOC: NewOrder.flags |= IOC delegated to the engine; the residual is detected
 * via ExecResult.filled and emitted as a CancelAck for the unfilled remainder.
 */
#include "matching_engine_api.h"

#include "lob/book_core.hpp"
#include "lob/price_levels.hpp"
#include "lob/logging.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

using namespace lob;

// A 100,001-slot contiguous price band covers the canonical workloads'
// 10,494–42,817 tick envelope with generous margin while keeping the
// PriceLevelsContig footprint reasonable.
constexpr Tick MIN_TICK  = 0;
constexpr Tick MAX_TICK  = 100000;
constexpr Tick TICK_SIZE = 1;

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

std::unique_ptr<PriceLevelsContig> g_bids;
std::unique_ptr<PriceLevelsContig> g_asks;
std::unique_ptr<BookCore>          g_book;

/* PAYLOAD ECHO ONLY — {price, side} for the CancelAck, a real engine gap:
 * BookCore::cancel(id) returns a bare bool and log_cancel carries only
 * (id, ts). The reject DECISION is always the engine's: cancel(id)'s bool
 * and modify()'s {0,0} not-found result adjudicate every cancel/modify (the
 * engine's id index erases filled makers at fill time, so they answer
 * correctly for every lifecycle state). Harness ids are dense and 1-based,
 * so a flat vector indexed by order_id holds the echo with no per-insert
 * allocation on the timed path. */
struct Shadow {
    int64_t  price;
    uint8_t  side;       // 0 = buy, 1 = sell
};
std::vector<Shadow> g_shadow;

/* Bounds-checked flat-vector lookup. */
inline Shadow* find_order(uint64_t ext_id) {
    return ext_id < g_shadow.size() ? &g_shadow[ext_id] : nullptr;
}

// Per-call context: the harness message's seq, propagated into the engine's
// log_fill callback so Trade reports carry the originating message's seq.
// (The taker id itself arrives in the callback from the engine.)
uint64_t g_cur_seq    = 0;

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
        // No maker bookkeeping: the engine erases fully-filled makers from
        // its own id index at fill time, so a later cancel/modify of one
        // rejects through the engine's bool/{0,0} answer.
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
    /* resize (not reserve): zero-fills the slots (all fields zero) and faults
     * the pages in, both untimed. engine_prebuild grows past this if needed. */
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

    g_cur_seq = o->sequence_number;

    NewOrder no{};
    no.seq   = o->sequence_number;
    no.ts    = 0;
    no.id    = o->order_id;
    no.user  = o->order_id;   // uid is inert here: the STP flag is never set
    no.side  = (o->side == 0) ? Side::Bid : Side::Ask;
    no.price = static_cast<Tick>(o->price_ticks);
    no.qty   = static_cast<Quantity>(o->quantity);
    no.flags = (o->ioc ? IOC : NONE);

    ExecResult r = g_book->submit_limit(no);

    if (o->ioc) {
        // Native IOC: the engine reports the residual in ExecResult and
        // never rests it.
        const uint32_t residual =
            (r.filled < static_cast<Quantity>(o->quantity))
                ? static_cast<uint32_t>(o->quantity - r.filled)
                : 0u;
        if (residual > 0)
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id,
                     o->side, o->price_ticks, residual);
        return;
    }

    // GTC: shadow records {price, side} for the CancelAck echo. Liveness is
    // the engine's to answer — its id index has no entry for a fully-filled
    // order, so later cancels/modifies reject through the engine itself.
    g_shadow[o->order_id] = { o->price_ticks, o->side };
}

void engine_on_cancel(const cancel_t* c) {
    // The engine adjudicates: its id-keyed cancel(id) returns true iff the
    // order was resting (never seen, already cancelled and filled-away ids
    // all return false, with no side effects). The shadow only supplies the
    // side/price echo the bool cannot.
    if (!g_book->cancel(c->order_id)) {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
        return;
    }
    Shadow* s = find_order(c->order_id);   // defensive null: unreachable —
    emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id,   // engine-resting
             s ? s->side : 0, s ? s->price : 0, 0);            // => slot recorded
}

void engine_on_modify(const modify_t* m) {
    // mansoor's native modify() IS the harness contract: it erases the order,
    // matches any cross at the new price (fills emitted through log_fill,
    // stamped with this message's seq), and re-enqueues the remainder at the
    // TAIL of the new level — queue priority lost. The engine adjudicates:
    // ExecResult{0,0} is its not-found signal (the workload guarantees
    // new_qty > 0), returned with no side effects. The canonical stream
    // stable-sorts by (seq, type), so trades emitted inside modify() followed
    // by the ModifyAck hash identically to ack-first.
    g_cur_seq = m->sequence_number;
    ModifyOrder mo{};
    mo.seq       = m->sequence_number;
    mo.ts        = 0;
    mo.id        = m->order_id;
    mo.new_price = static_cast<Tick>(m->new_price_ticks);
    mo.new_qty   = static_cast<Quantity>(m->new_quantity);
    mo.flags     = NONE;
    ExecResult r = g_book->modify(mo);
    if (r.filled == 0 && r.remaining == 0) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }
    // The modify message itself carries the order's side (the ABI populates
    // modify_t.side) — use the message-native field; the shadow slot only
    // needs its price refreshed for a later CancelAck echo.
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             m->side, m->new_price_ticks, m->new_quantity);
    Shadow* s = find_order(m->order_id);   // defensive null: unreachable
    if (s) *s = { m->new_price_ticks, m->side };
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
