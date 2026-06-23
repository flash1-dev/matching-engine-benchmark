/*
 * pyxchange_adapter.cpp — matching_engine_api.h backed by PyXchange's C++ core.
 *
 * PyXchange: https://github.com/pavelschon/PyXchange  (35*). A limit-orderbook
 * matching engine whose core is C++ (a Boost.MultiIndex price-time book) with a
 * Boost.Python / Twisted server layer on top. This adapter drives the C++
 * matcher core directly (OrderBook / Order / OrderContainer) and ignores the
 * Python/Twisted layer entirely, exactly as the brief requires.
 *
 * The matching algorithm and data structure are used UNMODIFIED:
 *   - OrderContainer  (boost::multi_index_container, ordered_unique on
 *     (price,time) for the match walk, ordered_non_unique on price for depth,
 *     hashed_unique on (trader,orderId) for cancel) — order_container/.
 *   - OrderBook::handleExecution / insertOrder / cancelOrder<> — the price-time
 *     match loop, maker-price fills, FIFO time priority — orderbook/.
 *
 * What the engine patch (build.sh) changes is ONLY the I/O edges the C++ core
 * was welded to the Python runtime through, none of the matching logic:
 *   1. PyXchange.hpp drops <boost/python.hpp> (so the core builds without a
 *      Python interpreter) and makes prio_t a monotonic uint64 FIFO counter
 *      instead of a wall-clock time_point (see "time priority" below).
 *   2. Order gets a plain-typed constructor in place of the py::dict one.
 *   3. OrderBook gains non-Python entry points (newOrder / newOrderIOC /
 *      cancel, plus bestBid / bestAsk / depthAt for queries) that dispatch into
 *      the EXISTING private templated workers — the match logic is reached
 *      unchanged. Modify is a cancel + reinsert done adapter-side from these.
 *   4. The reporting edge is redirected to this adapter: the single internal
 *      per-fill hook OrderBook::notifyExecution() calls pyx_on_trade() below
 *      (the engine's native "one call per fill" point); the Trader / Client /
 *      Logger Python notify paths become no-ops (the market-data broadcast is
 *      the public tape, not the per-order report stream the harness wants).
 *
 * The adapter emits the report stream itself, mirroring the liquibook baseline:
 * an OrderAck per new order, a Trade per fill (via pyx_on_trade), a CancelAck
 * per cancel and per IOC residual, a ModifyAck per modify, and a CancelReject /
 * ModifyReject per cancel/modify of an order that is not resting. PyXchange
 * matches synchronously on the calling thread, so engine_flush() is a no-op.
 *
 * Order ids: the harness gives an order_id only (no trader); PyXchange keys a
 * resting order by (trader, orderId). The adapter uses a SINGLE synthetic
 * trader for every order, so (trader, orderId) collapses to order_id. PyXchange
 * has no self-match prevention (the selfMatch string in Constants.hpp is dead
 * code, and handleExecution never inspects trader identity — confirmed by the
 * engine's own TradingTest, which matches a trader against its own resting
 * orders), so one trader for all orders matches across them exactly as the
 * anonymous harness book intends.
 *
 * Time priority: PyXchange stamped Order::time with
 * std::chrono::high_resolution_clock::now(). The book's primary index is an
 * ordered_unique on (price,time); if two same-price orders ever get an equal
 * timestamp (clock resolution under a burst), the second insert() returns
 * .second == false and the engine drops the order as a duplicate. The patch
 * makes time a strictly-increasing monotonic counter — the FIFO priority the
 * engine already intends — which removes that latent tie/drop hazard
 * deterministically. (This is an adapter-side hardening of the priority source,
 * not a change to how matching works.)
 */

#include "orderbook/OrderBook.hpp"
#include "order_container/OrderContainer.hpp"   // complete type for ~OrderBook (unique_ptr members)
#include "client/Trader.hpp"
#include "utils/Side.hpp"

#include "matching_engine_api.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#if defined(__aarch64__)
static inline void cpu_pause() { asm volatile("yield" ::: "memory"); }
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
#else
static inline void cpu_pause() {}
#endif

namespace {

using pyxchange::OrderBook;
using pyxchange::Trader;
using pyxchange::TraderPtr;
using pyxchange::ClientVector;
using pyxchange::ClientVectorPtr;
using pyxchange::orderId_t;
using pyxchange::price_t;
using pyxchange::quantity_t;
using pyxchange::side_t;

const me_transport_t* g_transport = nullptr;     // harness report transport
void*                 g_sink      = nullptr;

std::unique_ptr<OrderBook> g_book;
ClientVectorPtr            g_clients;             // empty: no market-data clients
TraderPtr                  g_trader;              // single synthetic trader

uint64_t g_seq = 0;                               // aggressive order's seq (this call)

// Per-order liveness shadow indexed by harness order_id. The harness ids are
// dense and 1-based; nullptr/false = not resting. PyXchange keys cancels by
// (trader,orderId) and can answer "is it resting?" via its hashed index, but a
// flat shadow is the cheaper way to synthesize CancelReject/ModifyReject and to
// recover an order's side for the modify reinsert. Sized for capacity in
// engine_init (untimed); every write happens on the clock in engine_on_*.
struct Live { bool resting; uint8_t side; int64_t price; uint32_t qty; };
std::vector<Live> g_live;

inline void ensure_id(uint64_t id) {
    if (id >= g_live.size())
        g_live.resize(std::max<size_t>(g_live.size() * 2, id + 1), Live{false,0,0,0});
}

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) cpu_pause();
}

void emit_ack(uint8_t type, uint64_t seq, uint64_t order_id,
              uint8_t side, int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = type;
    r.sequence_number = seq;
    r.order_id        = order_id;
    r.side            = side;
    r.price_ticks     = price;
    r.quantity        = qty;
    push_report(r);
}

}  // namespace

/* ---------------------------------------------------------------------------
 * Engine reporting edge. OrderBook::notifyExecution() (patched in build.sh)
 * calls this exactly once per fill, with the aggressor (taker) and resting
 * (maker) order ids, the maker's price, and the matched quantity — the single
 * native per-fill point of the engine's match loop. We translate it 1:1 to a
 * harness Trade. The synthetic-trader scheme makes the order ids == harness
 * order ids directly.
 * ------------------------------------------------------------------------- */
extern "C" void pyx_on_trade(uint64_t taker_id, uint64_t maker_id,
                             int64_t price, uint32_t qty) {
    me_report_t r{};
    r.type            = ME_TRADE;
    r.sequence_number = g_seq;                    // aggressive order's seq
    r.price_ticks     = price;                    // maker's resting price
    r.quantity        = qty;
    r.maker_order_id  = maker_id;
    r.taker_order_id  = taker_id;
    push_report(r);

    // Keep the maker's liveness shadow current: a fully-consumed maker has been
    // erased from the book by handleExecution, so reflect that here.
    if (maker_id < g_live.size()) {
        Live& m = g_live[maker_id];
        if (m.resting) {
            if (qty >= m.qty) { m.resting = false; m.qty = 0; }
            else              { m.qty -= qty; }
        }
    }
}

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;

    g_clients = std::make_shared<ClientVector>();          // empty market-data set
    g_book    = std::make_unique<OrderBook>(g_clients);
    g_trader  = std::make_shared<Trader>("harness");       // one trader for all

    g_live.assign(1u << 21, Live{false, 0, 0, 0});         // capacity pre-size
}

void engine_shutdown(void) {
    g_book.reset();
    g_trader.reset();
    g_clients.reset();
    g_live.clear();
}

/* PyXchange matches synchronously on the calling thread — nothing is pending. */
void engine_flush(void) {}

void engine_on_new_order(const new_order_t* o) {
    g_seq = o->sequence_number;
    ensure_id(o->order_id);

    // OrderAck first, then the fills (mirrors the liquibook baseline ordering).
    emit_ack(ME_ORDER_ACK, o->sequence_number, o->order_id,
             o->side, o->price_ticks, o->quantity);

    // side: harness 0=buy/1=sell -> engine bid_=1/ask_=2.
    const side_t eside = (o->side == 0) ? pyxchange::side::bid_ : pyxchange::side::ask_;

    uint32_t filled = 0;
    if (o->ioc) {
        // IOC: match against the opposite book, never rest the residual.
        filled = g_book->newOrderIOC(g_trader, eside,
                                     static_cast<orderId_t>(o->order_id),
                                     static_cast<price_t>(o->price_ticks),
                                     static_cast<quantity_t>(o->quantity));
        // IOC never rests; shadow stays not-resting.
        if (filled < o->quantity)
            emit_ack(ME_CANCEL_ACK, o->sequence_number, o->order_id, o->side,
                     o->price_ticks, static_cast<uint32_t>(o->quantity - filled));
    } else {
        // Normal limit: match crossing quantity, rest the remainder.
        const quantity_t resting =
            g_book->newOrder(g_trader, eside,
                             static_cast<orderId_t>(o->order_id),
                             static_cast<price_t>(o->price_ticks),
                             static_cast<quantity_t>(o->quantity));
        if (resting > 0) {
            Live& l = g_live[o->order_id];
            l.resting = true;
            l.side    = o->side;
            l.price   = o->price_ticks;
            l.qty     = static_cast<uint32_t>(resting);
        }
    }
}

void engine_on_cancel(const cancel_t* c) {
    if (c->order_id < g_live.size() && g_live[c->order_id].resting) {
        Live& l = g_live[c->order_id];
        const bool removed = g_book->cancel(g_trader, static_cast<orderId_t>(c->order_id));
        (void)removed;   // shadow says resting => the engine has it
        emit_ack(ME_CANCEL_ACK, c->sequence_number, c->order_id, l.side, l.price, 0);
        l.resting = false;
        l.qty     = 0;
    } else {
        emit_ack(ME_CANCEL_REJECT, c->sequence_number, c->order_id, 0, 0, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    g_seq = m->sequence_number;

    if (!(m->order_id < g_live.size() && g_live[m->order_id].resting)) {
        emit_ack(ME_MODIFY_REJECT, m->sequence_number, m->order_id, 0, 0, 0);
        return;
    }

    // Modify = cancel + reinsert at the new price/quantity (loses time priority).
    g_book->cancel(g_trader, static_cast<orderId_t>(m->order_id));
    g_live[m->order_id].resting = false;

    const side_t eside = (m->side == 0) ? pyxchange::side::bid_ : pyxchange::side::ask_;
    const quantity_t resting =
        g_book->newOrder(g_trader, eside,
                         static_cast<orderId_t>(m->order_id),
                         static_cast<price_t>(m->new_price_ticks),
                         static_cast<quantity_t>(m->new_quantity));
    if (resting > 0) {
        Live& l = g_live[m->order_id];
        l.resting = true;
        l.side    = m->side;
        l.price   = m->new_price_ticks;
        l.qty     = static_cast<uint32_t>(resting);
    }
    emit_ack(ME_MODIFY_ACK, m->sequence_number, m->order_id,
             m->side, m->new_price_ticks, m->new_quantity);
}

int64_t engine_query_best_bid(void) {
    int64_t p;
    return g_book->bestBid(p) ? p : INT64_MIN;
}

int64_t engine_query_best_ask(void) {
    int64_t p;
    return g_book->bestAsk(p) ? p : INT64_MAX;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    const side_t eside = (side == 0) ? pyxchange::side::bid_ : pyxchange::side::ask_;
    return g_book->depthAt(static_cast<price_t>(price_ticks), eside);
}

// Batch delivery: loop the per-message handlers (inlined under -O3), same
// strict in-order semantics as one-at-a-time. PyXchange is a plain C++ matcher
// on the calling thread, so this only removes the per-message dispatch overhead.
void engine_on_batch(const me_msg_t* msgs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const me_msg_t& mm = msgs[i];
        if (mm.type == 0)      engine_on_new_order(&mm.no);
        else if (mm.type == 1) engine_on_cancel(&mm.c);
        else                   engine_on_modify(&mm.md);
    }
}

}  // extern "C"
