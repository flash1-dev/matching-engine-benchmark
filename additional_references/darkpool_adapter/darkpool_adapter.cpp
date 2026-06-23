/*
 * darkpool_adapter.cpp — dendisuhubdy/dark_pool's `ordermatch` sample matcher
 * behind the harness matching_engine_api.h ABI.
 *
 * The engine that actually compiles and runs in dark_pool is the QuickFIX-style
 * `ordermatch` book in src/ordermatch/: a price/time-priority limit order book
 * over two std::multimaps, driven by the Market class (Market::insert /
 * Market::match(queue&) / Market::find / Market::erase) over Order objects.
 * Orders are keyed inside the engine by a string clientID, so the adapter
 * marshals the harness's integer order_id into its decimal string form (exactly
 * as the engine's own FIX front-end carries a ClOrdID).
 *
 * Native API used:
 *   - Market::insert(Order)            — rest / present the incoming order
 *   - Market::match(std::queue&)        — the engine's own crossing loop; it
 *                                         pushes the two filled Order copies
 *                                         (bid then ask) per fill into the queue
 *   - Market::find(Side,id) / erase(Order) — native cancel / modify
 *
 * The adapter keeps a minimal per-order liveness shadow {side, price, openQty,
 * live} ONLY because the engine offers no result code telling the harness an
 * order is not resting (needed to synthesize CancelReject / ModifyReject) and
 * because the engine's find/erase are keyed by (side,id) while the harness
 * cancel/modify carry only order_id. No matching is reimplemented in the
 * adapter: every fill, and every executed price, comes from the engine.
 *
 * Trade reporting: per the contract, Trade.price_ticks is the MAKER's (resting)
 * price and maker/taker_order_id name the two orders. The maker is the order
 * already resting when the aggressor arrived — i.e. the order whose side is
 * opposite the incoming order's. The adapter reads the maker order's
 * engine-computed getLastExecutedPrice(); the price the engine stamps there is
 * the engine's own. (On the unpatched engine that price is wrong for the
 * resting-bid-vs-incoming-sell case — Market::match prices unconditionally at
 * the ask — which is exactly the divergence under test.)
 *
 * IOC: harness IOC orders match what they can; any unfilled remainder is
 * removed and reported as a CancelAck (the engine has no native IOC type, so
 * the residual is detected from the shadow and pulled from the book).
 */
#include "matching_engine_api.h"

#include "Market.h"          // engine: ordermatch Market + Order
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

Market g_market;                       // single symbol — the canonical workload
const std::string SYMBOL = "X";
const std::string OWNER  = "O";
const std::string TARGET = "T";

// Per-order liveness + locate shadow. Harness order ids are dense and 1-based,
// so a flat vector indexed by order_id holds it with no per-insert allocation
// on the timed path.
struct Shadow {
    int64_t  price = 0;     // resting price in ticks
    uint8_t  side  = 0;     // 0 = buy, 1 = sell
    bool     live  = false; // resting in the book right now
};
std::vector<Shadow> g_shadow;

inline Shadow& shadow_for(uint64_t oid) {
    if (oid >= g_shadow.size()) g_shadow.resize(oid + 1024);
    return g_shadow[oid];
}

inline void emit(uint8_t type, uint8_t side, uint64_t seq, uint64_t oid,
                 int64_t price, uint32_t qty,
                 uint64_t maker = 0, uint64_t taker = 0) {
    me_report_t r;
    r.type            = type;
    r.side            = side;
    r._reserved[0] = r._reserved[1] = r._reserved[2] = 0;
    r._reserved[3] = r._reserved[4] = r._reserved[5] = 0;
    r.sequence_number = seq;
    r.order_id        = oid;
    r.price_ticks     = price;
    r.quantity        = qty;
    r._reserved2      = 0;
    r.maker_order_id  = maker;
    r.taker_order_id  = taker;
    r._reserved3      = 0;
    g_transport->push(g_sink, &r);
}

// order_id <-> decimal-string clientID, the engine's native key form.
inline std::string id_to_str(uint64_t id) { return std::to_string(id); }
inline uint64_t    str_to_id(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

inline Order::Side eng_side(uint8_t s) { return s == 0 ? Order::buy : Order::sell; }

// Reusable fill queue for the engine's crossing loop. Hoisted to file scope so
// drain_matches() does not construct/destroy a fresh std::queue (std::deque,
// which heap-allocates its first block on construction and frees it on
// destruction) on every engine_on_new_order / engine_on_modify on the timed
// hot path. Cleared by drain-to-empty at the top of each call, never realloc'd.
std::queue<Order> g_fills;

// Drive the engine's crossing loop after an order has been inserted, and turn
// each fill the engine produces into a Trade report. `aggr_id`/`aggr_side` name
// the just-inserted aggressor so the resting (maker) side can be identified for
// the maker-price + maker/taker ids the contract requires.
void drain_matches(uint64_t aggr_id, uint8_t aggr_side, uint64_t aggr_seq) {
    // Reuse the file-scope queue; Market::match(queue&) only ever push()es, so
    // clear any residual (drain to empty) before handing it to the engine. The
    // pair-walk loop below also leaves it empty, so this is normally a no-op.
    std::queue<Order>& fills = g_fills;
    while (!fills.empty()) fills.pop();
    g_market.match(fills);
    // The engine pushes the two filled Order copies per crossing: bid then ask
    // (Market::match(queue&) does orders.push(bid); orders.push(ask)). Walk
    // them in pairs.
    while (fills.size() >= 2) {
        Order bid = fills.front(); fills.pop();   // engine pushes bid first,
        Order ask = fills.front(); fills.pop();   // then ask, per crossing.

        uint64_t bid_id = str_to_id(bid.getClientID());
        uint64_t ask_id = str_to_id(ask.getClientID());

        // Maker = the resting side = the copy that is NOT the aggressor.
        const Order* maker;
        uint64_t maker_id, taker_id;
        if (ask_id == aggr_id) {            // aggressor is the ask → maker is the bid
            maker = &bid; maker_id = bid_id; taker_id = ask_id;
        } else {                            // aggressor is the bid → maker is the ask
            maker = &ask; maker_id = ask_id; taker_id = bid_id;
        }

        // Fill price = the MAKER's engine-computed last execution price; the
        // per-fill quantity is the same on both copies.
        int64_t  fill_price = static_cast<int64_t>(maker->getLastExecutedPrice());
        uint32_t fill_qty   = static_cast<uint32_t>(maker->getLastExecutedQuantity());

        emit(ME_TRADE, aggr_side, aggr_seq, taker_id, fill_price, fill_qty,
             maker_id, taker_id);

        // If the engine closed the maker, it is no longer resting.
        if (maker->isClosed()) shadow_for(maker_id).live = false;
    }
}

} // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport, void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;
    g_market    = Market();
    g_shadow.clear();
    g_shadow.resize(1 << 16);
}

void engine_shutdown(void) {}

void engine_on_new_order(const new_order_t* o) {
    const uint64_t oid  = o->order_id;
    const uint64_t seq  = o->sequence_number;
    const int64_t  px   = o->price_ticks;
    const uint8_t  side = o->side;

    // OrderAck first (one per accepted new order).
    emit(ME_ORDER_ACK, side, seq, oid, px, o->quantity);

    Order order(id_to_str(oid), SYMBOL, OWNER, TARGET,
                eng_side(side), Order::limit,
                static_cast<double>(px), static_cast<long>(o->quantity));
    g_market.insert(order);

    Shadow& s = shadow_for(oid);
    s.price = px; s.side = side; s.live = true;

    // Let the engine cross; report each fill it makes.
    drain_matches(oid, side, seq);

    // If the incoming order fully filled, it is no longer resting.
    // The engine removed it inside match() when it closed; reflect that.
    // (We detect via the shadow update done in drain_matches for the maker;
    //  for the taker/aggressor we check the book directly below.)
    Order* still = nullptr;
    try { still = &g_market.find(eng_side(side), id_to_str(oid)); }
    catch (...) { still = nullptr; }
    if (!still) s.live = false;

    // IOC: any unfilled remainder must be removed and reported as a CancelAck.
    if (o->ioc) {
        if (s.live) {
            // Pull the residual from the book and cancel it.
            try {
                Order& resting = g_market.find(eng_side(side), id_to_str(oid));
                uint32_t rem = static_cast<uint32_t>(resting.getOpenQuantity());
                g_market.erase(resting);
                s.live = false;
                emit(ME_CANCEL_ACK, side, seq, oid, px, rem);
            } catch (...) {
                emit(ME_CANCEL_ACK, side, seq, oid, px, 0);
            }
        } else {
            // Fully filled IOC: no residual, no CancelAck.
        }
    }
}

void engine_on_cancel(const cancel_t* c) {
    const uint64_t oid = c->order_id;
    const uint64_t seq = c->sequence_number;
    Shadow& s = shadow_for(oid);
    if (!s.live) {
        emit(ME_CANCEL_REJECT, s.side, seq, oid, s.price, 0);
        return;
    }
    try {
        Order& resting = g_market.find(eng_side(s.side), id_to_str(oid));
        uint32_t rem = static_cast<uint32_t>(resting.getOpenQuantity());
        g_market.erase(resting);
        s.live = false;
        emit(ME_CANCEL_ACK, s.side, seq, oid, s.price, rem);
    } catch (...) {
        s.live = false;
        emit(ME_CANCEL_REJECT, s.side, seq, oid, s.price, 0);
    }
}

void engine_on_modify(const modify_t* m) {
    const uint64_t oid = m->order_id;
    const uint64_t seq = m->sequence_number;
    Shadow& s = shadow_for(oid);
    if (!s.live) {
        emit(ME_MODIFY_REJECT, m->side, seq, oid, s.price, 0);
        return;
    }
    // Modify = cancel + reinsert at the new price/qty (losing time priority).
    try {
        Order& resting = g_market.find(eng_side(s.side), id_to_str(oid));
        g_market.erase(resting);
    } catch (...) {
        s.live = false;
        emit(ME_MODIFY_REJECT, m->side, seq, oid, s.price, 0);
        return;
    }

    const int64_t npx = m->new_price_ticks;
    Order reins(id_to_str(oid), SYMBOL, OWNER, TARGET,
                eng_side(s.side), Order::limit,
                static_cast<double>(npx), static_cast<long>(m->new_quantity));
    g_market.insert(reins);
    s.price = npx; s.live = true;

    // The reinsert may cross — report any fills, with this order as aggressor.
    drain_matches(oid, s.side, seq);

    Order* still = nullptr;
    try { still = &g_market.find(eng_side(s.side), id_to_str(oid)); }
    catch (...) { still = nullptr; }
    if (!still) s.live = false;

    emit(ME_MODIFY_ACK, s.side, seq, oid, npx, m->new_quantity);
}

void engine_flush(void) {}

int64_t engine_query_best_bid(void) {
    // Walk the shadow? No — query the engine. The engine has no public best-bid
    // accessor, so derive from the live shadow set.
    int64_t best = std::numeric_limits<int64_t>::min();
    for (size_t i = 0; i < g_shadow.size(); ++i) {
        const Shadow& s = g_shadow[i];
        if (s.live && s.side == 0 && s.price > best) best = s.price;
    }
    return best;
}

int64_t engine_query_best_ask(void) {
    int64_t best = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < g_shadow.size(); ++i) {
        const Shadow& s = g_shadow[i];
        if (s.live && s.side == 1 && s.price < best) best = s.price;
    }
    return best;
}

uint64_t engine_query_depth_at(int64_t price_ticks, uint8_t side) {
    uint64_t total = 0;
    // Aggregate resting open quantity at the level from the engine's books.
    // The engine has no depth accessor, so sum live orders at that price.
    // We need open quantity per order — fetch from the engine by id.
    for (size_t i = 0; i < g_shadow.size(); ++i) {
        const Shadow& s = g_shadow[i];
        if (s.live && s.side == side && s.price == price_ticks) {
            try {
                Order& o = g_market.find(eng_side(side), id_to_str((uint64_t)i));
                total += (uint64_t)o.getOpenQuantity();
            } catch (...) {}
        }
    }
    return total;
}

} // extern "C"
